#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WiFi credential staging error codes
 * Used by net_mgr_test_and_commit_credentials()
 */
typedef enum {
    NET_MGR_CRED_OK = 0,              // Success
    NET_MGR_CRED_AUTH_FAILED,         // Wrong password
    NET_MGR_CRED_AP_NOT_FOUND,        // SSID not found
    NET_MGR_CRED_TIMEOUT,             // Connection timeout
    NET_MGR_CRED_INVALID_INPUT,       // Invalid SSID/password
    NET_MGR_CRED_BUSY,                // Another operation in progress
    NET_MGR_CRED_UNKNOWN_ERROR        // Other failure
} net_mgr_cred_result_t;

esp_err_t net_mgr_start(void);
esp_err_t net_mgr_reconnect(void);  // Apply new credentials from config and reconnect
bool net_mgr_is_ready(void);  // Check if network has IP (silent, no warnings)
esp_err_t net_mgr_get_ip(char* ip_str, size_t ip_str_len);
esp_err_t net_mgr_get_mac(char* mac_str, size_t mac_str_len);
esp_err_t net_mgr_get_netmask(char* netmask_str, size_t netmask_str_len);
esp_err_t net_mgr_get_gateway(char* gw_str, size_t gw_str_len);
int net_mgr_get_rssi(int* rssi_out);

/**
 * Test WiFi credentials and commit to NVS only on success
 *
 * @param ssid New SSID to test
 * @param pass New password to test
 * @param timeout_ms Connection timeout in milliseconds
 * @param result_out Pointer to receive result code (success/failure reason)
 *
 * @return ESP_OK if test successful and credentials committed
 *         ESP_ERR_* on failure (credentials rolled back)
 */
esp_err_t net_mgr_test_and_commit_credentials(const char* ssid, const char* pass,
                                                uint32_t timeout_ms,
                                                net_mgr_cred_result_t* result_out);

/**
 * Convert credential result to user-friendly string
 * @param result Result code from test_and_commit_credentials
 * @return Error code string for JSON response, or NULL for success
 */
const char* net_mgr_cred_result_to_string(net_mgr_cred_result_t result);

#ifdef __cplusplus
}
#endif
