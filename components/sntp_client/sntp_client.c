/* SNTP Client - Network-aware time synchronization
 * Follows event-driven pattern from ntrip_client.c
 */

#include "sntp_client.h"
#include "config_mgr.h"
#include "event_bus.h"
#include "net_mgr.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "sntp_client";

// State
static volatile sntp_status_t s_status = SNTP_STATUS_IDLE;
static time_t s_last_sync_time = 0;
static TaskHandle_t s_sntp_task = NULL;
static bool s_should_run = false;

// Event group for config changes and network state
static EventGroupHandle_t s_sntp_event_group = NULL;
#define SNTP_CONFIG_CHANGED_BIT BIT0
#define SNTP_NETWORK_READY_BIT  BIT1

// Current configuration (cached)
static char s_server_primary[128] = {0};
static char s_server_secondary[128] = {0};
static char s_timezone[64] = {0};

/**
 * Time sync notification callback
 * Called by ESP-IDF when time is synchronized
 */
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized via SNTP");
    s_last_sync_time = tv->tv_sec;
    s_status = SNTP_STATUS_SYNCED;

    // Apply timezone
    if (strlen(s_timezone) > 0) {
        ESP_LOGI(TAG, "Applying timezone: %s", s_timezone);
        setenv("TZ", s_timezone, 1);
        tzset();
    } else {
        ESP_LOGI(TAG, "No timezone configured, using UTC");
        setenv("TZ", "UTC0", 1);
        tzset();
    }

    // Log current time for verification
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);
}

/**
 * Load SNTP configuration from NVS
 */
static esp_err_t load_config(void)
{
    esp_err_t ret;

    // Load primary server (NVS key: sntp/server1)
    ret = config_mgr_get_string("sntp/server1", s_server_primary, sizeof(s_server_primary));
    if (ret != ESP_OK || strlen(s_server_primary) == 0) {
        ESP_LOGW(TAG, "Primary NTP server not configured, using default: pool.ntp.org");
        strncpy(s_server_primary, "pool.ntp.org", sizeof(s_server_primary) - 1);
    } else {
        // Validate server string (basic hostname/IP check)
        if (strlen(s_server_primary) > 63) {
            ESP_LOGW(TAG, "Primary NTP server name too long, using default");
            strncpy(s_server_primary, "pool.ntp.org", sizeof(s_server_primary) - 1);
        }
    }

    // Load secondary server (NVS key: sntp/server2)
    ret = config_mgr_get_string("sntp/server2", s_server_secondary, sizeof(s_server_secondary));
    if (ret != ESP_OK || strlen(s_server_secondary) == 0) {
        ESP_LOGW(TAG, "Secondary NTP server not configured, using default: time.google.com");
        strncpy(s_server_secondary, "time.google.com", sizeof(s_server_secondary) - 1);
    } else {
        // Validate server string (basic hostname/IP check)
        if (strlen(s_server_secondary) > 63) {
            ESP_LOGW(TAG, "Secondary NTP server name too long, using default");
            strncpy(s_server_secondary, "time.google.com", sizeof(s_server_secondary) - 1);
        }
    }

    // Load timezone
    ret = config_mgr_get_string("sntp/timezone", s_timezone, sizeof(s_timezone));
    if (ret != ESP_OK || strlen(s_timezone) == 0) {
        ESP_LOGW(TAG, "Timezone not configured, using UTC");
        strncpy(s_timezone, "UTC0", sizeof(s_timezone) - 1);
    }

    ESP_LOGI(TAG, "SNTP config loaded - Primary: %s, Secondary: %s, TZ: %s",
             s_server_primary, s_server_secondary, s_timezone);

    return ESP_OK;
}

/**
 * Initialize SNTP with current configuration
 */
static esp_err_t init_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");

    // Configure SNTP
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                               ESP_SNTP_SERVER_LIST(s_server_primary, s_server_secondary));
    config.sync_cb = time_sync_notification_cb;
    config.smooth_sync = true;  // Smooth time adjustment instead of step

    // Initialize SNTP
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(ret));
        s_status = SNTP_STATUS_ERROR;
        return ret;
    }

    ESP_LOGI(TAG, "SNTP initialized successfully");
    s_status = SNTP_STATUS_SYNCING;

    return ESP_OK;
}

/**
 * Deinitialize SNTP
 */
static void deinit_sntp(void)
{
    ESP_LOGI(TAG, "Deinitializing SNTP...");
    esp_netif_sntp_deinit();
    s_status = SNTP_STATUS_IDLE;
}

/**
 * Event handler for network state changes
 * - NET_READY: Set network ready bit to allow sync attempts
 * - NET_LOST: Clear network ready bit to pause sync attempts
 */
static void sntp_network_event_handler(void* arg, esp_event_base_t event_base,
                                       int32_t event_id, void* event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == DEVICE_EVENT) {
        if (event_id == DEVICE_EVENT_NET_READY) {
            ESP_LOGI(TAG, "Network ready, allowing SNTP sync");
            if (s_sntp_event_group) {
                xEventGroupSetBits(s_sntp_event_group, SNTP_NETWORK_READY_BIT);
            }
        } else if (event_id == DEVICE_EVENT_NET_LOST) {
            ESP_LOGW(TAG, "Network lost, pausing SNTP sync");
            if (s_sntp_event_group) {
                xEventGroupClearBits(s_sntp_event_group, SNTP_NETWORK_READY_BIT);
            }
        }
    }
}

/**
 * SNTP client task
 * Waits for network ready, initializes SNTP, monitors for config changes
 */
static void sntp_client_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SNTP client task started");

    while (s_should_run) {
        // Load configuration
        if (load_config() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load config, waiting 10s...");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        // Check for config change request
        EventBits_t bits = xEventGroupGetBits(s_sntp_event_group);
        bool config_changed = (bits & SNTP_CONFIG_CHANGED_BIT) != 0;
        if (config_changed) {
            ESP_LOGI(TAG, "Config change detected - applying immediately");
            xEventGroupClearBits(s_sntp_event_group, SNTP_CONFIG_CHANGED_BIT);
        }

        // Wait for network if not ready
        bits = xEventGroupGetBits(s_sntp_event_group);
        if (!(bits & SNTP_NETWORK_READY_BIT)) {
            if (!s_should_run) {
                break;  // Exit if we're shutting down
            }

            ESP_LOGI(TAG, "Network not ready, waiting for NET_READY event...");
            s_status = SNTP_STATUS_IDLE;

            // Block indefinitely until network ready bit is set (or config change)
            xEventGroupWaitBits(s_sntp_event_group,
                                SNTP_NETWORK_READY_BIT | SNTP_CONFIG_CHANGED_BIT,
                                pdFALSE,  // Don't clear bits
                                pdFALSE,  // Wait for ANY bit (OR)
                                portMAX_DELAY);

            if (!s_should_run) {
                break;  // Exit if we're shutting down
            }

            continue;  // Reload config and try again
        }

        // Initialize SNTP
        if (init_sntp() != ESP_OK) {
            ESP_LOGE(TAG, "SNTP initialization failed, retrying in 30s...");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        // Monitor for config changes or network loss
        while (s_should_run) {
            // Check event bits
            bits = xEventGroupWaitBits(s_sntp_event_group,
                                       SNTP_CONFIG_CHANGED_BIT | SNTP_NETWORK_READY_BIT,
                                       pdFALSE,  // Don't clear bits
                                       pdFALSE,  // Wait for ANY bit (OR)
                                       pdMS_TO_TICKS(60000));  // Check every minute

            if (!s_should_run) {
                break;
            }

            // Config change detected
            if (bits & SNTP_CONFIG_CHANGED_BIT) {
                ESP_LOGI(TAG, "Config change detected - restarting SNTP");
                xEventGroupClearBits(s_sntp_event_group, SNTP_CONFIG_CHANGED_BIT);
                deinit_sntp();
                break;  // Exit inner loop to reload config
            }

            // Network lost
            if (!(bits & SNTP_NETWORK_READY_BIT)) {
                ESP_LOGW(TAG, "Network lost - stopping SNTP");
                deinit_sntp();
                break;  // Exit inner loop to wait for network
            }

            // Update sync status based on ESP-IDF's internal state
            sntp_sync_status_t sync_status = sntp_get_sync_status();
            if (sync_status == SNTP_SYNC_STATUS_COMPLETED) {
                if (s_status != SNTP_STATUS_SYNCED) {
                    s_status = SNTP_STATUS_SYNCED;
                    ESP_LOGI(TAG, "SNTP sync completed");
                }
            } else if (sync_status == SNTP_SYNC_STATUS_IN_PROGRESS) {
                if (s_status != SNTP_STATUS_SYNCING) {
                    s_status = SNTP_STATUS_SYNCING;
                }
            }
        }

        // Deinitialize before restarting
        deinit_sntp();
    }

    ESP_LOGI(TAG, "SNTP client task exiting");
    s_sntp_task = NULL;
    vTaskDelete(NULL);
}

/**
 * Start SNTP client
 */
esp_err_t sntp_client_start(void)
{
    if (s_sntp_task != NULL) {
        ESP_LOGW(TAG, "SNTP client already started");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting SNTP client");

    // Create event group
    s_sntp_event_group = xEventGroupCreate();
    if (!s_sntp_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Register network event handlers
    esp_err_t ret = esp_event_handler_register(DEVICE_EVENT, DEVICE_EVENT_NET_READY,
                                                &sntp_network_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register NET_READY handler: %s", esp_err_to_name(ret));
        vEventGroupDelete(s_sntp_event_group);
        s_sntp_event_group = NULL;
        return ret;
    }

    ret = esp_event_handler_register(DEVICE_EVENT, DEVICE_EVENT_NET_LOST,
                                     &sntp_network_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register NET_LOST handler: %s", esp_err_to_name(ret));
        esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_READY, &sntp_network_event_handler);
        vEventGroupDelete(s_sntp_event_group);
        s_sntp_event_group = NULL;
        return ret;
    }

    // Check if network is already ready
    char ip_check[16];
    if (net_mgr_get_ip(ip_check, sizeof(ip_check)) == ESP_OK && strcmp(ip_check, "0.0.0.0") != 0) {
        ESP_LOGI(TAG, "Network already ready (IP: %s), setting ready bit", ip_check);
        xEventGroupSetBits(s_sntp_event_group, SNTP_NETWORK_READY_BIT);
    } else {
        ESP_LOGI(TAG, "Network not ready, waiting for NET_READY event");
    }

    // Create task
    s_should_run = true;
    BaseType_t task_ret = xTaskCreate(sntp_client_task, "sntp_client", 4096, NULL, 5, &s_sntp_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SNTP task");
        esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_READY, &sntp_network_event_handler);
        esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_LOST, &sntp_network_event_handler);
        vEventGroupDelete(s_sntp_event_group);
        s_sntp_event_group = NULL;
        s_should_run = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SNTP client started successfully");
    return ESP_OK;
}

/**
 * Stop SNTP client
 */
esp_err_t sntp_client_stop(void)
{
    if (s_sntp_task == NULL) {
        ESP_LOGW(TAG, "SNTP client not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping SNTP client");

    // Unregister network event handlers
    esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_READY, &sntp_network_event_handler);
    esp_event_handler_unregister(DEVICE_EVENT, DEVICE_EVENT_NET_LOST, &sntp_network_event_handler);

    // Signal task to stop
    s_should_run = false;
    if (s_sntp_event_group) {
        xEventGroupSetBits(s_sntp_event_group, SNTP_CONFIG_CHANGED_BIT);  // Wake up task
    }

    // Wait for task to exit (max 5 seconds)
    int wait_count = 0;
    while (s_sntp_task != NULL && wait_count < 50) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    if (s_sntp_task != NULL) {
        ESP_LOGW(TAG, "SNTP task did not exit cleanly, deleting forcefully");
        vTaskDelete(s_sntp_task);
        s_sntp_task = NULL;
    }

    // Clean up event group
    if (s_sntp_event_group) {
        vEventGroupDelete(s_sntp_event_group);
        s_sntp_event_group = NULL;
    }

    s_status = SNTP_STATUS_IDLE;
    ESP_LOGI(TAG, "SNTP client stopped");

    return ESP_OK;
}

/**
 * Reload SNTP configuration
 */
esp_err_t sntp_client_reload_config(void)
{
    if (s_sntp_task == NULL || s_sntp_event_group == NULL) {
        ESP_LOGW(TAG, "SNTP client not running, cannot reload config");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Signaling SNTP client to reload config");
    xEventGroupSetBits(s_sntp_event_group, SNTP_CONFIG_CHANGED_BIT);

    return ESP_OK;
}

/**
 * Get current SNTP status
 */
esp_err_t sntp_client_get_status(sntp_status_t* status_out)
{
    if (!status_out) {
        return ESP_ERR_INVALID_ARG;
    }

    *status_out = s_status;
    return ESP_OK;
}

/**
 * Get last sync time
 */
esp_err_t sntp_client_get_last_sync_time(time_t* time_out)
{
    if (!time_out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_last_sync_time == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *time_out = s_last_sync_time;
    return ESP_OK;
}

/**
 * Get current timezone string
 */
esp_err_t sntp_client_get_timezone(char* tz_buf, size_t tz_buf_len)
{
    if (!tz_buf || tz_buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(tz_buf, s_timezone, tz_buf_len - 1);
    tz_buf[tz_buf_len - 1] = '\0';

    return ESP_OK;
}
