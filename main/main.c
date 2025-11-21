/* ESP-NG - ESP32 Framework with Componentized Architecture
 * Minimal main.c demonstrating usage of reusable components
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

// Component headers
#include "version.h"
#include "app_startup.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-NG Framework Starting...");
    ESP_LOGI(TAG, "Version: %s", version_get_string());

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize networking stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Run generic startup (Phases 1-3)
    // This initializes: event_bus, config_mgr, wdt_mgr, net_mgr,
    // provisioning, sntp_client, http_ui, ota_mgr, udp_broadcast
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
