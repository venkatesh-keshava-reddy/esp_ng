#include "app_startup.h"
#include "esp_log.h"
#include "config_store.h"
#include "event_bus.h"
#include "config_mgr.h"
#include "wdt_mgr.h"
#include "net_mgr.h"
#include "provisioning_mgr.h"
#include "sntp_client.h"
#include "http_ui.h"
#include "ota_mgr.h"
#include "udp_broadcast.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_startup";

void app_startup_run_generic(void) {
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Generic Startup Orchestration");
    ESP_LOGI(TAG, "====================================");

    // ========================================
    // Phase 1: Core Infrastructure (no dependencies)
    // ========================================
    ESP_LOGI(TAG, "Phase 1: Initializing core infrastructure...");

    // Initialize NVS storage first (config_store wraps NVS)
    ESP_ERROR_CHECK(config_store_init());
    ESP_LOGI(TAG, "  [✓] Config store initialized (NVS)");

    ESP_ERROR_CHECK(event_bus_init());
    ESP_LOGI(TAG, "  [✓] Event bus initialized");

    ESP_ERROR_CHECK(config_mgr_init());
    ESP_LOGI(TAG, "  [✓] Config manager initialized");

#ifndef CONFIG_RUN_UNIT_TESTS
    // Watchdog manager (skipped if already initialized for unit tests)
    ESP_ERROR_CHECK(wdt_mgr_init());
    ESP_LOGI(TAG, "  [✓] Watchdog manager initialized");
#else
    ESP_LOGI(TAG, "  [✓] Watchdog manager (already initialized for tests)");
#endif

    // ========================================
    // Phase 2: Network Layer
    // ========================================
    ESP_LOGI(TAG, "Phase 2: Starting network layer...");

    ESP_ERROR_CHECK(net_mgr_start());
    ESP_LOGI(TAG, "  [✓] Network manager started");

    // Provisioning may fail if BLE is not enabled - log warning instead of crashing
    esp_err_t prov_ret = provisioning_mgr_start_if_needed();
    if (prov_ret != ESP_OK) {
        ESP_LOGW(TAG, "  [!] Provisioning skipped/failed: %s", esp_err_to_name(prov_ret));
    } else {
        ESP_LOGI(TAG, "  [✓] Provisioning manager started");
    }

    // ========================================
    // Phase 3: Network-Dependent Services
    // ========================================
    ESP_LOGI(TAG, "Phase 3: Starting network-dependent services...");

    // Start SNTP client (network-aware, event-driven)
    esp_err_t err = sntp_client_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  [!] SNTP client failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "  [✓] SNTP client started");
    }

    // Start HTTP UI (may fail if default password is in use)
    err = http_ui_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  [!] HTTP UI failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "  [!] HTTP UI disabled due to weak password");
        }
    } else {
        ESP_LOGI(TAG, "  [✓] HTTP UI started");
    }

    // Initialize OTA manager
    ESP_ERROR_CHECK(ota_mgr_init());
    ESP_LOGI(TAG, "  [✓] OTA manager initialized");

    // Start UDP broadcast
    ESP_ERROR_CHECK(udp_broadcast_start());
    ESP_LOGI(TAG, "  [✓] UDP broadcast started");

    // ========================================
    // Generic Startup Complete
    // ========================================
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Generic Startup Complete");
    ESP_LOGI(TAG, "Ready for application-specific init");
    ESP_LOGI(TAG, "====================================");
}

bool app_startup_is_time_synced(void) {
    // Query SNTP client for sync status
    sntp_status_t status;
    if (sntp_client_get_status(&status) == ESP_OK) {
        return (status == SNTP_STATUS_SYNCED);
    }
    return false;
}

esp_err_t app_startup_wait_for_time_sync(uint32_t timeout_ms) {
    ESP_LOGI(TAG, "Waiting for time sync (timeout: %lu ms)...", timeout_ms);

    uint32_t elapsed_ms = 0;
    const uint32_t poll_interval_ms = 100;

    while (elapsed_ms < timeout_ms) {
        sntp_status_t status;
        if (sntp_client_get_status(&status) == ESP_OK && status == SNTP_STATUS_SYNCED) {
            ESP_LOGI(TAG, "Time sync completed");
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        elapsed_ms += poll_interval_ms;
    }

    ESP_LOGW(TAG, "Time sync timeout after %lu ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}
