/**
 * @file wdt_mgr.h
 * @brief Task Watchdog Manager
 *
 * Provides a centralized watchdog management system using ESP-IDF's TWDT.
 * Implements bark/bite mechanism:
 * - Bark: Warning event when a task fails to feed the watchdog
 * - Bite: System reboot after 3 CONSECUTIVE barks without recovery
 *
 * Recovery mechanism:
 * - The bark counter is reset to 0 when any task successfully feeds
 * - This ensures only consecutive failures trigger a bite, not lifetime failures
 * - If tasks recover and resume feeding, they avoid the bite/reboot
 *
 * Usage pattern for tasks:
 * @code
 * void my_task(void *arg) {
 *     // Register with watchdog
 *     wdt_mgr_register_task("my_task", xTaskGetCurrentTaskHandle(), 10000);
 *
 *     while (1) {
 *         // Do work...
 *
 *         // Feed watchdog periodically (must be within timeout period)
 *         wdt_mgr_feed("my_task");
 *
 *         vTaskDelay(pdMS_TO_TICKS(2000));
 *     }
 * }
 * @endcode
 *
 * Critical tasks that should register:
 * - net_mgr (if it has a background task)
 * - ntrip_client (connection management task)
 * - udp_broadcast (broadcast_task)
 *
 * Tasks excluded (with rationale):
 * - http_ui: Uses ESP-IDF httpd thread pool with built-in timeout management,
 *   no long-running dedicated task that could block indefinitely
 */
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the watchdog manager
 *
 * Sets up the Task Watchdog Timer (TWDT) with the configured timeout.
 * Must be called after the scheduler starts and before registering tasks.
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_ERR_INVALID_STATE: Already initialized
 *  - Other: TWDT initialization failed
 */
esp_err_t wdt_mgr_init(void);

/**
 * @brief Register a task with the watchdog
 *
 * Tasks must call this before using wdt_mgr_feed(). The timeout_ms parameter
 * is for informational purposes only; the actual timeout is global and set
 * via CONFIG_ESP_TASK_WDT_TIMEOUT_S in sdkconfig.
 *
 * @param name Unique name for this task (max 31 chars)
 * @param handle Task handle or NULL for current task
 * @param timeout_ms Expected feed interval in ms (informational)
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_ERR_INVALID_STATE: wdt_mgr not initialized
 *  - ESP_ERR_INVALID_ARG: Invalid name
 *  - ESP_ERR_NO_MEM: Too many tasks registered (max 8)
 *  - Other: TWDT registration failed
 */
esp_err_t wdt_mgr_register_task(const char* name, TaskHandle_t handle, int timeout_ms);

/**
 * @brief Feed the watchdog for a named task
 *
 * Tasks must call this periodically (within the configured timeout) to
 * prevent watchdog events. A successful feed resets the bark counter to 0,
 * allowing the system to recover from previous failures.
 *
 * Safe to call from any context.
 *
 * @param name Task name used in wdt_mgr_register_task()
 */
void wdt_mgr_feed(const char* name);

/**
 * @brief Unregister a task from the watchdog
 *
 * Removes a task from watchdog monitoring. Tasks should call this before
 * exiting to clean up resources properly.
 *
 * @param name Task name used in wdt_mgr_register_task()
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_ERR_NOT_FOUND: Task not found
 *  - Other: TWDT unregistration failed
 */
esp_err_t wdt_mgr_unregister_task(const char* name);

#ifdef __cplusplus
}
#endif
