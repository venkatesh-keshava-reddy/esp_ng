#include "event_bus.h"
#include "esp_event.h"
#include "esp_log.h"

ESP_EVENT_DEFINE_BASE(DEVICE_EVENT);

static const char *TAG = "event_bus";

esp_err_t event_bus_init(void) {
    ESP_LOGI(TAG, "Event bus initialized (using default loop)");
    // Default loop is created in app_main; custom loops can be added later.
    return ESP_OK;
}

esp_err_t event_bus_post(esp_event_base_t base, int32_t id, void* data, size_t len, TickType_t timeout) {
    (void)timeout; // not used with esp_event_post
    return esp_event_post(base, id, data, len, portMAX_DELAY);
}
