#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run generic application startup (Phases 1-3)
 *
 * Orchestrates the standard startup sequence for generic components:
 * - Phase 1: Core infrastructure (event_bus, config_mgr, wdt_mgr)
 * - Phase 2: Network layer (net_mgr, provisioning)
 * - Phase 3: Network-dependent services (SNTP, HTTP UI, OTA, UDP broadcast)
 *
 * This function ensures all dependencies are met before starting each subsystem.
 * After this completes, applications can add their own Phase 4+ initialization
 * (e.g., GNSS, NTRIP, custom services).
 *
 * @note This is a blocking function that will abort on critical errors.
 *       Non-critical errors (e.g., provisioning, SNTP) are logged as warnings.
 */
void app_startup_run_generic(void);

/**
 * @brief Check if time has been synchronized via NTP
 *
 * Useful for applications that need time sync before TLS operations.
 *
 * @return true if time is synced, false otherwise
 */
bool app_startup_is_time_synced(void);

/**
 * @brief Wait for time synchronization with timeout
 *
 * This function blocks until time is synced or timeout expires.
 * Useful for ensuring time is valid before TLS connections.
 *
 * Example usage for NTRIP with TLS:
 * @code
 * if (use_tls && !app_startup_is_time_synced()) {
 *     esp_err_t err = app_startup_wait_for_time_sync(30000);
 *     if (err == ESP_OK) {
 *         // Time synced, proceed with TLS connection
 *     }
 * }
 * @endcode
 *
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK if time synced, ESP_ERR_TIMEOUT if timeout, ESP_FAIL on error
 */
esp_err_t app_startup_wait_for_time_sync(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
