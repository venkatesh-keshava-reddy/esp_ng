/* WiFi Network Manager - Adapted from ESP-IDF station example
 * Reference: D:\esp\esp-idf\examples\wifi\getting_started\station\
 */

#include "net_mgr.h"
#include "config_mgr.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "net_mgr";

// Event bits for connection state
#define WIFI_CONNECTED_BIT BIT0

// Reconnection parameters (exponential backoff with cap)
#define BACKOFF_BASE_MS    1000
#define BACKOFF_MAX_MS     60000
#define MAX_RETRY_BEFORE_RESTART 20  // Full WiFi restart after 20 attempts

static esp_netif_t* s_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static int s_retry_num = 0;
static bool s_is_connected = false;

// Credential staging infrastructure
#define CRED_STAGING_CONNECTED_BIT   BIT0
#define CRED_STAGING_GOT_IP_BIT      BIT1
#define CRED_STAGING_FAILED_BIT      BIT2
#define CRED_STAGING_SUCCESS_BITS    (CRED_STAGING_CONNECTED_BIT | CRED_STAGING_GOT_IP_BIT)

static EventGroupHandle_t s_cred_staging_event_group = NULL;
static SemaphoreHandle_t s_cred_staging_mutex = NULL;
static volatile bool s_cred_staging_active = false;
static volatile wifi_err_reason_t s_cred_staging_fail_reason = WIFI_REASON_UNSPECIFIED;

/**
 * Timer callback for reconnection attempts
 * Runs in timer task context, not event loop - safe to call esp_wifi_connect()
 */
static void reconnect_timer_callback(void* arg)
{
    (void)arg;

    // Check if we've exceeded max retries - perform full WiFi restart
    if (s_retry_num >= MAX_RETRY_BEFORE_RESTART) {
        ESP_LOGW(TAG, "Max retry attempts (%d) exceeded, performing full WiFi restart", MAX_RETRY_BEFORE_RESTART);

        // Defensive WiFi restart from timer context
        // Stop WiFi - log but continue if it fails (may already be stopped)
        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "WiFi stop failed: %s (continuing anyway)", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief delay for cleanup

        // Restart WiFi - critical failure if this doesn't work
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi restart failed: %s - will retry on next attempt", esp_err_to_name(ret));
            // Don't reset retry counter - will attempt restart again
            // Increment so we don't get stuck in immediate restart loop
            s_retry_num++;
        } else {
            ESP_LOGI(TAG, "WiFi restarted successfully");
            // Reset retry counter only on successful restart
            s_retry_num = 0;
        }
        return;
    }

    ESP_LOGI(TAG, "Attempting reconnection (retry %d/%d)", s_retry_num + 1, MAX_RETRY_BEFORE_RESTART);
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
    }

    // Increment retry counter for next backoff calculation
    s_retry_num++;
}

/**
 * Schedule reconnection with exponential backoff
 * Non-blocking, uses esp_timer instead of vTaskDelay
 */
static void schedule_reconnect(void)
{
    // Calculate exponential backoff with jitter
    // Use uint64_t to prevent overflow when s_retry_num is large
    uint64_t backoff_ms = (uint64_t)BACKOFF_BASE_MS << s_retry_num;
    if (backoff_ms > BACKOFF_MAX_MS) {
        backoff_ms = BACKOFF_MAX_MS;
    }

    // Add jitter (0-1000ms) to avoid thundering herd
    uint32_t jitter = esp_random() % 1000;
    backoff_ms += jitter;

    ESP_LOGI(TAG, "Disconnected, scheduling reconnect in %llu ms (retry %d)",
             backoff_ms, s_retry_num + 1);

    // Stop any existing timer
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }

    // Start one-shot timer for reconnection
    esp_err_t ret = esp_timer_start_once(s_reconnect_timer, backoff_ms * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start reconnect timer: %s", esp_err_to_name(ret));
        // Fallback: try immediate reconnect
        esp_wifi_connect();
    }
}

/**
 * Event handler for Wi-Fi and IP events
 * Based on ESP-IDF station example event_handler()
 * IMPORTANT: Non-blocking - no vTaskDelay() or long operations
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started, connecting...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;

        // Post NET_LOST event with timeout to avoid blocking Wi-Fi task
        // If queue is full, event is dropped (non-critical for reconnection logic)
        event_bus_post(DEVICE_EVENT, DEVICE_EVENT_NET_LOST, NULL, 0, pdMS_TO_TICKS(100));

        // Schedule reconnection with exponential backoff (non-blocking)
        // Continues indefinitely with backoff capped at 60s
        schedule_reconnect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Stop any pending reconnection timer
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }

        // Reset retry counter on successful connection
        s_retry_num = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Post NET_READY event with timeout to avoid blocking Wi-Fi task
        event_bus_post(DEVICE_EVENT, DEVICE_EVENT_NET_READY, NULL, 0, pdMS_TO_TICKS(100));
    }
}

/**
 * Event handler for credential staging
 * Only active during test_and_commit operation
 */
static void cred_staging_event_handler(void* arg, esp_event_base_t event_base,
                                        int32_t event_id, void* event_data)
{
    (void)arg;

    if (!s_cred_staging_active) {
        return;  // Ignore events when not staging
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "[Staging] WiFi connected");
        if (s_cred_staging_event_group) {
            xEventGroupSetBits(s_cred_staging_event_group, CRED_STAGING_CONNECTED_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "[Staging] Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_cred_staging_event_group) {
            xEventGroupSetBits(s_cred_staging_event_group, CRED_STAGING_GOT_IP_BIT);
        }

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        s_cred_staging_fail_reason = event->reason;
        ESP_LOGW(TAG, "[Staging] Disconnected, reason: %d", event->reason);
        if (s_cred_staging_event_group) {
            xEventGroupSetBits(s_cred_staging_event_group, CRED_STAGING_FAILED_BIT);
        }
    }
}

esp_err_t net_mgr_start(void)
{
    esp_err_t ret;

    // Create event group
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create reconnection timer
    if (!s_reconnect_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = &reconnect_timer_callback,
            .name = "net_reconnect"
        };
        ret = esp_timer_create(&timer_args, &s_reconnect_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create reconnect timer: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Create staging mutex
    if (!s_cred_staging_mutex) {
        s_cred_staging_mutex = xSemaphoreCreateMutex();
        if (!s_cred_staging_mutex) {
            ESP_LOGE(TAG, "Failed to create credential staging mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Create default Wi-Fi STA interface (from example)
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) {
            ESP_LOGE(TAG, "Failed to create netif");
            return ESP_FAIL;
        }
    }

    // Initialize Wi-Fi with default config (from example)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers (adapted from example)
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(ret));
        return ret;
    }

    // Load Wi-Fi credentials from config_mgr (CLAUDE_TASKS.md requirement)
    char ssid[32] = {0};
    char password[64] = {0};

    ret = config_mgr_get_string("wifi/ssid", ssid, sizeof(ssid));
    if (ret != ESP_OK || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "No Wi-Fi SSID configured, skipping connection");
        ESP_LOGI(TAG, "Set Wi-Fi mode to STA for provisioning readiness");

        // Set mode anyway so provisioning can work
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret == ESP_OK) {
            ret = esp_wifi_start();
        }
        return ret;
    }

    ret = config_mgr_get_string("wifi/pass", password, sizeof(password));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No Wi-Fi password found, using empty password");
        password[0] = '\0';
    }

    // Configure Wi-Fi (adapted from example wifi_config_t setup)
    wifi_config_t wifi_config = {0};
    strlcpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    // Use WPA2 as minimum auth mode (from example)
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Network manager started, connecting to SSID: %s", ssid);

    // Clear password from stack memory (security best practice)
    memset(password, 0, sizeof(password));

    return ESP_OK;
}

esp_err_t net_mgr_reconnect(void)
{
    ESP_LOGI(TAG, "Reconnecting with new credentials");

    // Load new credentials from config_mgr
    char ssid[32] = {0};
    char password[64] = {0};

    esp_err_t ret = config_mgr_get_string("wifi/ssid", ssid, sizeof(ssid));
    if (ret != ESP_OK || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "No Wi-Fi SSID configured for reconnect");
        return ESP_ERR_INVALID_STATE;
    }

    ret = config_mgr_get_string("wifi/pass", password, sizeof(password));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No Wi-Fi password found, using empty password");
        password[0] = '\0';
    }

    // Disconnect from current network
    ESP_LOGI(TAG, "Disconnecting from current network");
    ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect warning: %s", esp_err_to_name(ret));
        // Continue anyway - might not be connected
    }

    // Configure new Wi-Fi credentials
    wifi_config_t wifi_config = {0};
    strlcpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    // Use WPA2 as minimum auth mode
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        memset(password, 0, sizeof(password));
        return ret;
    }

    // Clear password from stack memory
    memset(password, 0, sizeof(password));

    // Reset retry counter for new connection attempt
    s_retry_num = 0;

    // Reconnect with new credentials
    ESP_LOGI(TAG, "Connecting to new SSID: %s", ssid);
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

bool net_mgr_is_ready(void)
{
    if (!s_netif) {
        return false;
    }

    esp_netif_ip_info_t ip;
    esp_err_t ret = esp_netif_get_ip_info(s_netif, &ip);
    if (ret != ESP_OK) {
        return false;
    }

    // Check if IP is assigned (not 0.0.0.0)
    return (ip.ip.addr != 0);
}

esp_err_t net_mgr_get_ip(char* ip_str, size_t ip_str_len)
{
    if (!ip_str || ip_str_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_netif) {
        ESP_LOGE(TAG, "Network interface not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip;
    esp_err_t ret = esp_netif_get_ip_info(s_netif, &ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ip.ip.addr == 0) {
        ESP_LOGW(TAG, "No IP address assigned yet");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(ip_str, ip_str_len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

esp_err_t net_mgr_get_mac(char* mac_str, size_t mac_str_len)
{
    if (!mac_str || mac_str_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    snprintf(mac_str, mac_str_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

int net_mgr_get_rssi(int* rssi_out)
{
    if (!rssi_out) {
        return -1;
    }

    if (!s_is_connected) {
        return -1;
    }

    wifi_ap_record_t ap = {0};
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap);
    if (ret == ESP_OK) {
        *rssi_out = ap.rssi;
        return 0;
    }

    ESP_LOGW(TAG, "Failed to get RSSI: %s", esp_err_to_name(ret));
    return -1;
}

esp_err_t net_mgr_get_netmask(char* netmask_str, size_t netmask_str_len)
{
    if (!netmask_str || netmask_str_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_netif) {
        ESP_LOGE(TAG, "Network interface not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip;
    esp_err_t ret = esp_netif_get_ip_info(s_netif, &ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ip.netmask.addr == 0) {
        ESP_LOGW(TAG, "No netmask assigned yet");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(netmask_str, netmask_str_len, IPSTR, IP2STR(&ip.netmask));
    return ESP_OK;
}

esp_err_t net_mgr_get_gateway(char* gw_str, size_t gw_str_len)
{
    if (!gw_str || gw_str_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_netif) {
        ESP_LOGE(TAG, "Network interface not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip;
    esp_err_t ret = esp_netif_get_ip_info(s_netif, &ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ip.gw.addr == 0) {
        ESP_LOGW(TAG, "No gateway assigned yet");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(gw_str, gw_str_len, IPSTR, IP2STR(&ip.gw));
    return ESP_OK;
}

const char* net_mgr_cred_result_to_string(net_mgr_cred_result_t result)
{
    switch (result) {
        case NET_MGR_CRED_OK:
            return NULL;  // No error string for success
        case NET_MGR_CRED_AUTH_FAILED:
            return "wifi_auth_failed";
        case NET_MGR_CRED_AP_NOT_FOUND:
            return "wifi_ap_not_found";
        case NET_MGR_CRED_TIMEOUT:
            return "wifi_connect_timeout";
        case NET_MGR_CRED_INVALID_INPUT:
            return "wifi_invalid_input";
        case NET_MGR_CRED_BUSY:
            return "wifi_busy";
        case NET_MGR_CRED_UNKNOWN_ERROR:
        default:
            return "wifi_unknown_error";
    }
}

esp_err_t net_mgr_test_and_commit_credentials(const char* ssid, const char* pass,
                                                uint32_t timeout_ms,
                                                net_mgr_cred_result_t* result_out)
{
    esp_err_t ret = ESP_OK;
    net_mgr_cred_result_t result = NET_MGR_CRED_OK;

    // Step 1: Input Validation
    if (!ssid || !pass || !result_out) {
        if (result_out) *result_out = NET_MGR_CRED_INVALID_INPUT;
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);

    if (ssid_len == 0 || ssid_len > 32) {
        ESP_LOGE(TAG, "Invalid SSID length: %zu (must be 1-32)", ssid_len);
        *result_out = NET_MGR_CRED_INVALID_INPUT;
        return ESP_ERR_INVALID_ARG;
    }

    if (pass_len > 64) {
        ESP_LOGE(TAG, "Invalid password length: %zu (max 64)", pass_len);
        *result_out = NET_MGR_CRED_INVALID_INPUT;
        return ESP_ERR_INVALID_ARG;
    }

    // Step 2: Acquire Staging Mutex
    if (!s_cred_staging_mutex) {
        ESP_LOGE(TAG, "Staging mutex not initialized");
        *result_out = NET_MGR_CRED_UNKNOWN_ERROR;
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cred_staging_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Credential staging already in progress");
        *result_out = NET_MGR_CRED_BUSY;
        return ESP_ERR_INVALID_STATE;
    }

    // Step 3: Capture Current Credentials (for rollback)
    char old_ssid[33] = {0};
    char old_pass[65] = {0};

    ret = config_mgr_get_string("wifi/ssid", old_ssid, sizeof(old_ssid));
    if (ret != ESP_OK || strlen(old_ssid) == 0) {
        ESP_LOGE(TAG, "Failed to get current SSID for rollback");
        result = NET_MGR_CRED_UNKNOWN_ERROR;
        ret = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    config_mgr_get_string("wifi/pass", old_pass, sizeof(old_pass));

    ESP_LOGI(TAG, "[Staging] Testing credentials for SSID: %s", ssid);
    ESP_LOGI(TAG, "[Staging] Rollback SSID: %s", old_ssid);

    // Step 4: Initialize Staging Infrastructure
    if (!s_cred_staging_event_group) {
        s_cred_staging_event_group = xEventGroupCreate();
        if (!s_cred_staging_event_group) {
            ESP_LOGE(TAG, "Failed to create staging event group");
            result = NET_MGR_CRED_UNKNOWN_ERROR;
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    }

    xEventGroupClearBits(s_cred_staging_event_group, 0xFF);
    s_cred_staging_fail_reason = WIFI_REASON_UNSPECIFIED;
    s_cred_staging_active = true;

    // Register staging event handlers
    ret = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                       &cred_staging_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register staging WIFI_EVENT handler");
        result = NET_MGR_CRED_UNKNOWN_ERROR;
        goto cleanup_staging;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                       &cred_staging_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register staging IP_EVENT handler");
        result = NET_MGR_CRED_UNKNOWN_ERROR;
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                       &cred_staging_event_handler);
        goto cleanup_staging;
    }

    ret = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                       &cred_staging_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register staging DISCONNECT handler");
        result = NET_MGR_CRED_UNKNOWN_ERROR;
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                       &cred_staging_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                       &cred_staging_event_handler);
        goto cleanup_staging;
    }

    // Step 5: Apply New Credentials (RAM only)
    wifi_config_t new_wifi_config = {0};
    strlcpy((char*)new_wifi_config.sta.ssid, ssid, sizeof(new_wifi_config.sta.ssid));
    strlcpy((char*)new_wifi_config.sta.password, pass, sizeof(new_wifi_config.sta.password));
    new_wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    new_wifi_config.sta.pmf_cfg.capable = true;
    new_wifi_config.sta.pmf_cfg.required = false;

    ret = esp_wifi_set_config(WIFI_IF_STA, &new_wifi_config);
    memset(&new_wifi_config, 0, sizeof(new_wifi_config));  // Clear password

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set new WiFi config: %s", esp_err_to_name(ret));
        result = NET_MGR_CRED_UNKNOWN_ERROR;
        ret = ESP_FAIL;
        goto cleanup_handlers;
    }

    // Step 6: Trigger Connection Attempt
    ESP_LOGI(TAG, "[Staging] Disconnecting from current network");
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "[Staging] Connecting to new network");
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initiate connection: %s", esp_err_to_name(ret));
        result = NET_MGR_CRED_UNKNOWN_ERROR;
        goto cleanup_rollback;
    }

    // Step 7: Wait for Result (loop to accumulate both success bits)
    ESP_LOGI(TAG, "[Staging] Waiting up to %lu ms for connection...", timeout_ms);

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    EventBits_t accumulated_bits = 0;

    while (1) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks >= timeout_ticks) {
            // Timeout expired
            break;
        }

        TickType_t remaining_ticks = timeout_ticks - elapsed_ticks;

        // Wait for any bit to be set
        xEventGroupWaitBits(
            s_cred_staging_event_group,
            CRED_STAGING_SUCCESS_BITS | CRED_STAGING_FAILED_BIT,
            pdFALSE,  // Don't clear on exit
            pdFALSE,  // Wait for ANY bit
            remaining_ticks
        );

        // Accumulate bits
        accumulated_bits = xEventGroupGetBits(s_cred_staging_event_group);

        // Check for completion conditions
        if ((accumulated_bits & CRED_STAGING_SUCCESS_BITS) == CRED_STAGING_SUCCESS_BITS) {
            // Both success bits present
            break;
        }

        if (accumulated_bits & CRED_STAGING_FAILED_BIT) {
            // Failure bit present
            break;
        }

        // Continue waiting for more bits
    }

    EventBits_t bits = accumulated_bits;

    // Step 8: Evaluate Result
    if ((bits & CRED_STAGING_SUCCESS_BITS) == CRED_STAGING_SUCCESS_BITS) {
        // SUCCESS
        ESP_LOGI(TAG, "[Staging] SUCCESS - Committing credentials to NVS");

        config_mgr_set_string("wifi/ssid", ssid);
        config_mgr_set_string("wifi/pass", pass);

        result = NET_MGR_CRED_OK;
        ret = ESP_OK;

    } else if (bits & CRED_STAGING_FAILED_BIT) {
        // FAILURE
        ESP_LOGW(TAG, "[Staging] FAILED - Disconnect reason: %d", s_cred_staging_fail_reason);

        switch (s_cred_staging_fail_reason) {
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_AUTH_EXPIRE:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                result = NET_MGR_CRED_AUTH_FAILED;
                break;
            case WIFI_REASON_NO_AP_FOUND:
            case WIFI_REASON_BEACON_TIMEOUT:
                result = NET_MGR_CRED_AP_NOT_FOUND;
                break;
            default:
                result = NET_MGR_CRED_UNKNOWN_ERROR;
                break;
        }

        ret = ESP_FAIL;
        goto cleanup_rollback;

    } else {
        // TIMEOUT
        ESP_LOGW(TAG, "[Staging] TIMEOUT - No response within %lu ms", timeout_ms);
        result = NET_MGR_CRED_TIMEOUT;
        ret = ESP_ERR_TIMEOUT;
        goto cleanup_rollback;
    }

    // Success path
    goto cleanup_handlers;

cleanup_rollback:
    // Step 9: Rollback to Old Credentials
    ESP_LOGI(TAG, "[Staging] Rolling back to SSID: %s", old_ssid);

    wifi_config_t old_wifi_config = {0};
    strlcpy((char*)old_wifi_config.sta.ssid, old_ssid, sizeof(old_wifi_config.sta.ssid));
    strlcpy((char*)old_wifi_config.sta.password, old_pass, sizeof(old_wifi_config.sta.password));
    old_wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    old_wifi_config.sta.pmf_cfg.capable = true;
    old_wifi_config.sta.pmf_cfg.required = false;

    esp_err_t rollback_ret = esp_wifi_set_config(WIFI_IF_STA, &old_wifi_config);
    memset(&old_wifi_config, 0, sizeof(old_wifi_config));  // Clear password

    if (rollback_ret != ESP_OK) {
        ESP_LOGE(TAG, "[Staging] CRITICAL: Rollback failed: %s", esp_err_to_name(rollback_ret));
    }

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();

    ESP_LOGI(TAG, "[Staging] Rollback initiated, reconnecting to original network");

cleanup_handlers:
    // Unregister staging event handlers
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                   &cred_staging_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &cred_staging_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                   &cred_staging_event_handler);

cleanup_staging:
    s_cred_staging_active = false;

cleanup:
    memset(old_pass, 0, sizeof(old_pass));  // Clear password from stack
    xSemaphoreGive(s_cred_staging_mutex);

    *result_out = result;
    return ret;
}
