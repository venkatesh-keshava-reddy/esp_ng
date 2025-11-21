#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SNTP Client - Network-aware time synchronization
 *
 * Event-driven pattern:
 * - Starts sync when DEVICE_EVENT_NET_READY received
 * - Stops sync when DEVICE_EVENT_NET_LOST received
 * - Reloads config when signaled via sntp_client_reload_config()
 *
 * Uses ESP-IDF's esp_netif_sntp component with:
 * - Primary and secondary NTP servers for redundancy
 * - Automatic timezone application after sync
 * - Exponential backoff on sync failures
 */

/**
 * SNTP sync status
 */
typedef enum {
    SNTP_STATUS_IDLE = 0,      // Not started or stopped
    SNTP_STATUS_SYNCING,       // Attempting to sync
    SNTP_STATUS_SYNCED,        // Successfully synced
    SNTP_STATUS_ERROR          // Sync failed
} sntp_status_t;

/**
 * Start SNTP client
 * - Registers for NET_READY/NET_LOST events
 * - Creates background task
 * - Loads config from NVS
 * - Waits for network before attempting sync
 *
 * @return ESP_OK on success
 */
esp_err_t sntp_client_start(void);

/**
 * Stop SNTP client
 * - Unregisters event handlers
 * - Stops background task
 * - Deinitializes SNTP
 *
 * @return ESP_OK on success
 */
esp_err_t sntp_client_stop(void);

/**
 * Reload SNTP configuration from NVS and restart sync
 * Called after user updates config via HTTP API
 * Non-blocking - signals task via event group
 *
 * @return ESP_OK on success
 */
esp_err_t sntp_client_reload_config(void);

/**
 * Get current SNTP sync status
 *
 * @param status_out Pointer to receive status
 * @return ESP_OK on success
 */
esp_err_t sntp_client_get_status(sntp_status_t* status_out);

/**
 * Get time of last successful sync
 *
 * @param time_out Pointer to receive last sync time (Unix timestamp)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if never synced
 */
esp_err_t sntp_client_get_last_sync_time(time_t* time_out);

/**
 * Get current timezone string
 *
 * @param tz_buf Buffer to receive timezone string
 * @param tz_buf_len Buffer length
 * @return ESP_OK on success
 */
esp_err_t sntp_client_get_timezone(char* tz_buf, size_t tz_buf_len);

#ifdef __cplusplus
}
#endif
