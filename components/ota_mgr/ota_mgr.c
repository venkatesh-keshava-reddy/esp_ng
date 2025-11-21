#include "ota_mgr.h"
#include "config_mgr.h"
#include "event_bus.h"
#include "net_mgr.h"
#include "version.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "ota_mgr";

#define OTA_TASK_STACK_SIZE (8192)
#define OTA_TASK_PRIORITY (5)
#define HASH_LEN (32)

// OTA synchronization - prevent multiple simultaneous OTA operations
static SemaphoreHandle_t s_ota_mutex = NULL;
static volatile bool s_ota_in_progress = false;

// OTA task parameters
typedef struct {
    char url[256];
} ota_task_params_t;

// Static function to print SHA256 hash
static void print_sha256(const uint8_t *image_hash, const char *label)
{
    if (image_hash == NULL || label == NULL) {
        return;
    }

    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        snprintf(&hash_print[i * 2], 3, "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s: %s", label, hash_print);
}

// Static function to get and print partition SHA256
static void log_partition_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return;
    }

    ESP_LOGI(TAG, "Running partition: type %d, subtype %d, address 0x%lx, size 0x%lx, label %s",
             running->type, running->subtype, running->address, running->size, running->label);

    // Get and print running app info
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
        ESP_LOGI(TAG, "Running firmware project name: %s", running_app_info.project_name);
        ESP_LOGI(TAG, "Running firmware compile time: %s %s", running_app_info.date, running_app_info.time);
    }

    // Get and print SHA256 of running partition
    uint8_t sha_256[HASH_LEN] = {0};
    if (esp_partition_get_sha256(running, sha_256) == ESP_OK) {
        print_sha256(sha_256, "SHA-256 for current firmware");
    }
}

// HTTP event handler for OTA
static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

// OTA task - runs in separate task to avoid blocking
static void ota_task(void *pvParameter)
{
    if (pvParameter == NULL) {
        ESP_LOGE(TAG, "OTA task parameter is NULL");
        s_ota_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
        vTaskDelete(NULL);
        return;
    }

    ota_task_params_t *params = (ota_task_params_t *)pvParameter;
    esp_err_t ret = ESP_OK;
    esp_err_t ota_result = ESP_OK;
    esp_https_ota_handle_t https_ota_handle = NULL;
    bool ota_started = false;

    ESP_LOGI(TAG, "Starting OTA update from URL: %s", params->url);

    // Post OTA_BEGIN event
    event_bus_post(DEVICE_EVENT, DEVICE_EVENT_OTA_BEGIN, NULL, 0, pdMS_TO_TICKS(100));

    // Network pre-flight check - prevent DNS/HTTP failures if network is down
    char ip_check[16] = "0.0.0.0";
    if (net_mgr_get_ip(ip_check, sizeof(ip_check)) != ESP_OK ||
        strcmp(ip_check, "0.0.0.0") == 0) {
        ESP_LOGE(TAG, "Network unavailable (no IP address), cannot download OTA");
        ret = ESP_ERR_INVALID_STATE;
        goto ota_fail;
    }
    ESP_LOGI(TAG, "Network ready (IP: %s), proceeding with OTA download", ip_check);

    // Configure HTTP client
    esp_http_client_config_t http_config = {
        .url = params->url,
        .event_handler = ota_http_event_handler,
        .keep_alive_enable = true,
        .timeout_ms = 5000,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    // Configure HTTPS OTA
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Attempting to download update from %s", http_config.url);

    // Begin OTA operation
    ret = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed: %s", esp_err_to_name(ret));
        goto ota_fail;
    }
    ota_started = true;

    // Get image descriptor from new firmware
    esp_app_desc_t new_app_info;
    ret = esp_https_ota_get_img_desc(https_ota_handle, &new_app_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get image descriptor: %s", esp_err_to_name(ret));
        goto ota_fail;
    }

    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
    ESP_LOGI(TAG, "New firmware project: %s", new_app_info.project_name);

    // Optionally validate version (commented out for flexibility)
    // const esp_partition_t *running = esp_ota_get_running_partition();
    // esp_app_desc_t running_app_info;
    // if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
    //     if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0) {
    //         ESP_LOGW(TAG, "New version is same as running version, skipping");
    //         ret = ESP_FAIL;
    //         goto ota_fail;
    //     }
    // }

    // Perform OTA download in loop
    while (1) {
        ret = esp_https_ota_perform(https_ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // Monitor progress
        const size_t bytes_read = esp_https_ota_get_image_len_read(https_ota_handle);
        ESP_LOGD(TAG, "Image bytes read: %zu", bytes_read);
    }

    // Check if complete data received
    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true) {
        ESP_LOGE(TAG, "Complete data was not received");
        ret = ESP_FAIL;
        goto ota_fail;
    }

    // Finish OTA (validates and commits)
    ret = esp_https_ota_finish(https_ota_handle);
    https_ota_handle = NULL;  // Handle is freed by finish

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, preparing to reboot");

        // Record success in config_mgr with NEW firmware version
        config_mgr_set_u32("ota/last_result", (uint32_t)ESP_OK);
        config_mgr_set_string("ota/last_version", new_app_info.version);

        // Post OTA_SUCCESS event
        event_bus_post(DEVICE_EVENT, DEVICE_EVENT_OTA_SUCCESS, NULL, 0, pdMS_TO_TICKS(100));

        // Clean up
        free(params);
        s_ota_in_progress = false;
        xSemaphoreGive(s_ota_mutex);

        ESP_LOGI(TAG, "Rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "ESP HTTPS OTA finish failed: %s", esp_err_to_name(ret));
        goto ota_fail;
    }

    // Should never reach here due to restart
    vTaskDelete(NULL);
    return;

ota_fail:
    // Cleanup on failure
    if (ota_started && https_ota_handle != NULL) {
        esp_https_ota_abort(https_ota_handle);
    }

    ota_result = ret;
    ESP_LOGE(TAG, "OTA failed: %s (0x%x)", esp_err_to_name(ret), ret);

    // Record failure in config_mgr
    config_mgr_set_u32("ota/last_result", (uint32_t)ret);

    // Post OTA_FAIL event
    event_bus_post(DEVICE_EVENT, DEVICE_EVENT_OTA_FAIL, &ota_result, sizeof(ota_result), pdMS_TO_TICKS(100));

    // Clean up
    free(params);
    s_ota_in_progress = false;
    xSemaphoreGive(s_ota_mutex);

    // Task cleanup
    vTaskDelete(NULL);
}

esp_err_t ota_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager");

    // Create OTA mutex for synchronization
    if (s_ota_mutex == NULL) {
        s_ota_mutex = xSemaphoreCreateMutex();
        if (s_ota_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create OTA mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Validate OTA partitions exist
    const esp_partition_t *ota_0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota_1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);

    if (ota_0 == NULL || ota_1 == NULL) {
        ESP_LOGE(TAG, "OTA partitions not found in partition table");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "OTA partition 0: address 0x%lx, size 0x%lx, label %s",
             ota_0->address, ota_0->size, ota_0->label);
    ESP_LOGI(TAG, "OTA partition 1: address 0x%lx, size 0x%lx, label %s",
             ota_1->address, ota_1->size, ota_1->label);

    // Log current partition and firmware info
    log_partition_info();

    // Check if we're in rollback mode (optional - for advanced setups)
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGW(TAG, "OTA image is in pending verify state");
                ESP_LOGW(TAG, "Marking current app as valid to prevent rollback");
                esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
                if (mark_err == ESP_OK) {
                    ESP_LOGI(TAG, "App marked as valid, rollback cancelled");
                } else {
                    ESP_LOGW(TAG, "Failed to mark app as valid: %s", esp_err_to_name(mark_err));
                }
            } else if (ota_state == ESP_OTA_IMG_VALID) {
                ESP_LOGI(TAG, "Current OTA image is valid");
            } else if (ota_state == ESP_OTA_IMG_INVALID) {
                ESP_LOGW(TAG, "Current OTA image is marked as invalid");
            }
        }
    }

    ESP_LOGI(TAG, "OTA manager initialized successfully");
    return ESP_OK;
}

esp_err_t ota_mgr_trigger_from_url(const char* url)
{
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL provided");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(url) >= 256) {
        ESP_LOGE(TAG, "URL too long (max 255 characters)");
        return ESP_ERR_INVALID_SIZE;
    }

    // Check if OTA mutex was initialized
    if (s_ota_mutex == NULL) {
        ESP_LOGE(TAG, "OTA manager not initialized (mutex is NULL)");
        return ESP_ERR_INVALID_STATE;
    }

    // Try to acquire OTA mutex (non-blocking)
    if (xSemaphoreTake(s_ota_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "OTA operation already in progress, rejecting new request");
        return ESP_ERR_INVALID_STATE;
    }

    // Double-check flag (belt and suspenders)
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress (flag set), rejecting");
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // Mark OTA as in progress
    s_ota_in_progress = true;

    ESP_LOGI(TAG, "Triggering OTA update from URL: %s", url);

    // Allocate parameters for OTA task
    ota_task_params_t *params = malloc(sizeof(ota_task_params_t));
    if (params == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for OTA task parameters");
        s_ota_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Copy URL to params
    strncpy(params->url, url, sizeof(params->url) - 1);
    params->url[sizeof(params->url) - 1] = '\0';

    // Create OTA task
    BaseType_t task_created = xTaskCreate(
        &ota_task,
        "ota_task",
        OTA_TASK_STACK_SIZE,
        params,
        OTA_TASK_PRIORITY,
        NULL
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        free(params);
        s_ota_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA task created successfully");
    // Note: mutex will be released by ota_task on completion/failure
    return ESP_OK;
}
