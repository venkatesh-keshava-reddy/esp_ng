/* Provisioning Manager - BLE Wi-Fi Provisioning - Adapted from ESP-IDF wifi_prov_mgr example
 * Reference: D:\esp\esp-idf\examples\provisioning\wifi_prov_mgr\
 */

#include "provisioning_mgr.h"
#include "config_mgr.h"
#include "event_bus.h"
#include "net_mgr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include <string.h>

static const char *TAG = "prov_mgr";

// State
static bool s_is_provisioning = false;
static char s_pop[33] = {0};  // PoP derived from device_id (used directly as Security 1 param)

// Forward declarations
static void prov_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);

/**
 * Unregister all provisioning event handlers
 * Helper to ensure cleanup on all error paths
 */
static void unregister_event_handlers(void)
{
    esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler);
    esp_event_handler_unregister(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                  &prov_event_handler);
    esp_event_handler_unregister(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
                                  &prov_event_handler);
}

/**
 * Generate Proof of Possession from device_id
 * Uses hardcoded PoP for consistent provisioning
 */
static esp_err_t generate_pop_from_device_id(char* pop, size_t pop_len)
{
    if (!pop || pop_len < 9) {
        return ESP_ERR_INVALID_ARG;
    }

    // Hardcoded PoP for provisioning security
    strcpy(pop, "7E7BA724");

    ESP_LOGI(TAG, "Using PoP: %s", pop);
    return ESP_OK;
}

/**
 * Generate BLE service name from MAC address
 * Format: "PROV_XXXXXX" where X are last 3 bytes of MAC in hex
 */
static void generate_service_name(char* service_name, size_t max_len)
{
    uint8_t eth_mac[6];
    const char* prefix = "PROV_";

    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max_len, "%s%02X%02X%02X",
             prefix, eth_mac[3], eth_mac[4], eth_mac[5]);

    ESP_LOGI(TAG, "BLE service name: %s", service_name);
}

/**
 * Event handler for provisioning events
 */
static void prov_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;

            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t* wifi_sta_cfg = (wifi_sta_config_t*)event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials - SSID: %s",
                         (const char*)wifi_sta_cfg->ssid);
                // Note: Don't log password per security guardrails

                // Save credentials to NVS via config_mgr
                esp_err_t ret = config_mgr_set_string("wifi/ssid", (const char*)wifi_sta_cfg->ssid);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(ret));
                }

                ret = config_mgr_set_string("wifi/pass", (const char*)wifi_sta_cfg->password);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(ret));
                }

                ESP_LOGI(TAG, "Wi-Fi credentials saved to NVS");

                // Trigger WiFi reconnect with new credentials
                // Use net_mgr_reconnect() to avoid re-initializing WiFi driver
                ESP_LOGI(TAG, "Triggering WiFi reconnect with new credentials");
                ret = net_mgr_reconnect();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to reconnect with new credentials: %s", esp_err_to_name(ret));
                }

                break;
            }

            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t* reason = (wifi_prov_sta_fail_reason_t*)event_data;
                ESP_LOGE(TAG, "Provisioning failed: %s",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi authentication failed" : "Wi-Fi AP not found");
                break;
            }

            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                break;

            case WIFI_PROV_END:
                ESP_LOGI(TAG, "Provisioning ended, deinitializing manager");
                wifi_prov_mgr_deinit();
                unregister_event_handlers();  // Cleanup handlers to allow future provisioning
                s_is_provisioning = false;
                break;

            default:
                break;
        }
    } else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        switch (event_id) {
            case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
                ESP_LOGI(TAG, "BLE transport: Connected");
                break;

            case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
                ESP_LOGI(TAG, "BLE transport: Disconnected");
                break;

            default:
                break;
        }
    } else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        switch (event_id) {
            case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
                ESP_LOGI(TAG, "Secured session established");
                break;

            case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
                ESP_LOGE(TAG, "Invalid security parameters received");
                break;

            case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
                ESP_LOGE(TAG, "Incorrect PoP - credentials mismatch");
                break;

            default:
                break;
        }
    }
}

esp_err_t provisioning_mgr_start_if_needed(void)
{
    ESP_LOGI(TAG, "Checking provisioning status");

    // Check if WiFi credentials exist in config
    char ssid[33] = {0};
    esp_err_t ret = config_mgr_get_string("wifi/ssid", ssid, sizeof(ssid));

    if (ret == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Wi-Fi credentials found (SSID: %s), skipping provisioning", ssid);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "No Wi-Fi credentials found, starting provisioning");

    // Generate PoP from device_id
    ret = generate_pop_from_device_id(s_pop, sizeof(s_pop));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate PoP: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    ret = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                      &prov_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_PROV_EVENT handler: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                      &prov_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register BLE event handler: %s",
                 esp_err_to_name(ret));
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler);
        return ret;
    }

    ret = esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
                                      &prov_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PROTOCOMM event handler: %s",
                 esp_err_to_name(ret));
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler);
        esp_event_handler_unregister(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                      &prov_event_handler);
        return ret;
    }

    // Configuration for provisioning manager
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,  // Use BLE transport
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM  // Free BT/BLE after provisioning
    };

    // Initialize provisioning manager
    ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize provisioning manager: %s",
                 esp_err_to_name(ret));
        unregister_event_handlers();  // Cleanup handlers on error
        return ret;
    }

    // Check if already provisioned (via wifi_prov_mgr's internal state)
    bool provisioned = false;
    ret = wifi_prov_mgr_is_provisioned(&provisioned);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to check provisioning status: %s", esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        unregister_event_handlers();  // Cleanup handlers on error
        return ret;
    }

    if (provisioned) {
        ESP_LOGI(TAG, "Device already provisioned (via prov_mgr state), deinitializing");
        wifi_prov_mgr_deinit();
        unregister_event_handlers();  // Cleanup handlers (not an error, but exiting early)
        return ESP_OK;
    }

    // Generate BLE service name
    char service_name[12];
    generate_service_name(service_name, sizeof(service_name));

    // Set up Security 1 with PoP
    // In ESP-IDF, wifi_prov_security1_params_t is just const char* alias
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const char* sec_params = s_pop;  // PoP string directly
    const char* service_key = NULL;  // Not used for BLE

    // Set custom BLE service UUID (optional, for device identification)
    uint8_t custom_service_uuid[] = {
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };
    wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);

    // Start provisioning service
    ret = wifi_prov_mgr_start_provisioning(security, sec_params,
                                             service_name, service_key);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        unregister_event_handlers();  // Cleanup handlers on error
        return ret;
    }

    s_is_provisioning = true;
    ESP_LOGI(TAG, "Provisioning started - BLE service: %s, PoP: %s", service_name, s_pop);

    // Log QR code info for manual provisioning (optional)
    ESP_LOGI(TAG, "Scan QR code or use ESP BLE Prov app");
    ESP_LOGI(TAG, "Service: %s, PoP: %s", service_name, s_pop);

    return ESP_OK;
}

esp_err_t provisioning_mgr_stop(void)
{
    if (!s_is_provisioning) {
        ESP_LOGW(TAG, "Provisioning not active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping provisioning");

    // Unregister event handlers
    unregister_event_handlers();

    // Stop and deinit provisioning manager
    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();

    s_is_provisioning = false;

    return ESP_OK;
}
