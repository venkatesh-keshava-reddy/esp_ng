#include "wdt_mgr.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "wdt_mgr";

#define MAX_REGISTERED_TASKS 8
#define MAX_TASK_NAME_LEN 32
#define BARK_THRESHOLD 3  // Number of barks before triggering bite (reboot)

// Default timeout if not configured
#ifndef CONFIG_ESP_TASK_WDT_TIMEOUT_S
#define CONFIG_ESP_TASK_WDT_TIMEOUT_S 10
#endif

typedef struct {
    char name[MAX_TASK_NAME_LEN];
    TaskHandle_t handle;
    esp_task_wdt_user_handle_t wdt_handle;
    bool active;
} registered_task_t;

static registered_task_t s_registered_tasks[MAX_REGISTERED_TASKS];
static volatile int s_bark_count = 0;
static bool s_initialized = false;

// Forward declaration of ISR handler
void esp_task_wdt_isr_user_handler(void);

esp_err_t wdt_mgr_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "WDT manager already initialized");
        return ESP_OK;
    }

    // Clear registered tasks array
    memset(s_registered_tasks, 0, sizeof(s_registered_tasks));

    // Configure TWDT
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,  // Don't monitor idle tasks
        .trigger_panic = false,  // Don't panic on timeout, use bark/bite mechanism
    };

    esp_err_t ret = esp_task_wdt_init(&twdt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TWDT: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_bark_count = 0;

    ESP_LOGI(TAG, "WDT manager initialized (timeout=%u ms, bark_threshold=%d)",
             twdt_config.timeout_ms, BARK_THRESHOLD);

    return ESP_OK;
}

esp_err_t wdt_mgr_register_task(const char* name, TaskHandle_t handle, int timeout_ms) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "WDT manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (name == NULL) {
        ESP_LOGE(TAG, "Task name cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Check if task already registered
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (s_registered_tasks[i].active && strcmp(s_registered_tasks[i].name, name) == 0) {
            ESP_LOGW(TAG, "Task '%s' already registered", name);
            return ESP_OK;
        }
    }

    // Find empty slot
    int slot = -1;
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (!s_registered_tasks[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        ESP_LOGE(TAG, "No free slots for task registration (max=%d)", MAX_REGISTERED_TASKS);
        return ESP_ERR_NO_MEM;
    }

    // Register user with TWDT
    esp_task_wdt_user_handle_t wdt_handle;
    esp_err_t ret = esp_task_wdt_add_user(name, &wdt_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register task '%s' with TWDT: %s", name, esp_err_to_name(ret));
        return ret;
    }

    // Store task information
    strncpy(s_registered_tasks[slot].name, name, MAX_TASK_NAME_LEN - 1);
    s_registered_tasks[slot].name[MAX_TASK_NAME_LEN - 1] = '\0';
    s_registered_tasks[slot].handle = handle;
    s_registered_tasks[slot].wdt_handle = wdt_handle;
    s_registered_tasks[slot].active = true;

    ESP_LOGI(TAG, "Registered task '%s' with TWDT (timeout=%d ms)", name, timeout_ms);

    return ESP_OK;
}

void wdt_mgr_feed(const char* name) {
    if (!s_initialized) {
        return;
    }

    if (name == NULL) {
        return;
    }

    // Find task by name
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (s_registered_tasks[i].active && strcmp(s_registered_tasks[i].name, name) == 0) {
            esp_err_t ret = esp_task_wdt_reset_user(s_registered_tasks[i].wdt_handle);
            if (ret == ESP_OK) {
                // Successfully fed - reset bark counter to allow recovery
                // This ensures we only count consecutive barks, not lifetime barks
                if (s_bark_count > 0) {
                    ESP_LOGD(TAG, "Task '%s' recovered, resetting bark counter from %d to 0",
                             name, s_bark_count);
                }
                s_bark_count = 0;
            } else if (ret != ESP_ERR_NOT_FOUND) {
                // Only log if it's an actual error (not just task not found)
                ESP_LOGD(TAG, "Failed to feed watchdog for '%s': %s", name, esp_err_to_name(ret));
            }
            return;
        }
    }

    ESP_LOGD(TAG, "Task '%s' not found in registered tasks", name);
}

esp_err_t wdt_mgr_unregister_task(const char* name) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "WDT manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (name == NULL) {
        ESP_LOGE(TAG, "Task name cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Find and unregister task
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (s_registered_tasks[i].active && strcmp(s_registered_tasks[i].name, name) == 0) {
            esp_err_t ret = esp_task_wdt_delete_user(s_registered_tasks[i].wdt_handle);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to unregister task '%s' from TWDT: %s",
                         name, esp_err_to_name(ret));
                // Continue cleanup even if TWDT delete fails
            }

            // Clear slot
            s_registered_tasks[i].active = false;
            memset(&s_registered_tasks[i], 0, sizeof(registered_task_t));

            ESP_LOGI(TAG, "Unregistered task '%s' from TWDT", name);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Task '%s' not found in registered tasks", name);
    return ESP_ERR_NOT_FOUND;
}

// ISR handler for TWDT timeout (bark event)
// NOTE: This is called from ISR context - must be ISR-safe!
//
// Bark/bite semantics:
// - s_bark_count tracks CONSECUTIVE barks (failures to feed)
// - Counter is reset to 0 in wdt_mgr_feed() on successful feed (recovery)
// - Only 3 consecutive barks without any successful feeds trigger a bite (reboot)
// - This prevents lifetime accumulation and allows recovery
void esp_task_wdt_isr_user_handler(void) {
    s_bark_count++;

    // Take snapshot of volatile variable for ISR post (avoid volatile pointer warning)
    int bark = s_bark_count;

    // Post bark event (ISR-safe)
    esp_event_isr_post(DEVICE_EVENT, DEVICE_EVENT_WDT_BARK, &bark, sizeof(bark), NULL);

    // If consecutive bark count exceeds threshold, trigger bite (reboot)
    if (s_bark_count >= BARK_THRESHOLD) {
        // Take snapshot for bite event
        int bite = s_bark_count;

        // Post bite event before reboot
        esp_event_isr_post(DEVICE_EVENT, DEVICE_EVENT_WDT_BITE, &bite, sizeof(bite), NULL);

        // Note: We cannot use ESP_LOGE here (ISR context), so the bite event
        // handler should log the reboot reason
        // Trigger system reboot
        esp_restart();
    }
}
