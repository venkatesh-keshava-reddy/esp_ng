/* ESP-NG - ESP32 Framework with Componentized Architecture
 * Minimal main.c demonstrating usage of reusable components
 */

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

// Component headers
#include "version.h"
#include "app_startup.h"

#ifdef CONFIG_RUN_UNIT_TESTS
#include "test_harness.h"
#endif

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-NG Framework Starting...");
    ESP_LOGI(TAG, "Version: %s", version_get_string());

#ifdef CONFIG_RUN_UNIT_TESTS
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "           UNIT TEST MODE");
    ESP_LOGI(TAG, "========================================");

    // Initialize minimal services needed by tests
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Run component test harness with component grouping
    test_harness_run();

    // Reboot to re-enter test menu (no app startup in test builds)
    ESP_LOGI(TAG, "Test harness exited. Rebooting into test menu...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Brief delay to see the message
    esp_restart();
#endif

    // Initialize networking stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Run generic startup (Phases 1-3)
    // This initializes: config_store (NVS), event_bus, config_mgr, wdt_mgr,
    // net_mgr, provisioning, sntp_client, http_ui, ota_mgr, udp_broadcast
    app_startup_run_generic();

    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "Application-Specific Initialization");
    ESP_LOGI(TAG, "====================================");

    // Phase 4+: Your application-specific code goes here
    // For GNSS applications, you would add:
    // - GNSS manager initialization
    // - NTRIP client startup (with TLS time sync if needed)
    // - GNSS broadcast startup
    // - Custom application services
    //
    // Example for GNSS app:
    // if (gnss_enabled) {
    //     gnss_mgr_start();
    //     gnss_broadcast_start();
    //
    //     if (ntrip_configured && use_tls) {
    //         app_startup_wait_for_time_sync(30000);
    //     }
    //     ntrip_client_start();
    // }

    ESP_LOGI(TAG, "Application initialized successfully");
}
