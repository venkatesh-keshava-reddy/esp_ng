#pragma once
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(DEVICE_EVENT);

typedef enum {
    DEVICE_EVENT_NET_READY = 0,
    DEVICE_EVENT_NET_LOST,
    DEVICE_EVENT_NTRIP_CONNECTED,
    DEVICE_EVENT_NTRIP_DISCONNECTED,
    DEVICE_EVENT_UDP_STARTED,
    DEVICE_EVENT_UDP_STOPPED,
    DEVICE_EVENT_OTA_BEGIN,
    DEVICE_EVENT_OTA_SUCCESS,
    DEVICE_EVENT_OTA_FAIL,
    DEVICE_EVENT_WDT_BARK,
    DEVICE_EVENT_WDT_BITE,

    // GNSS events
    DEVICE_EVENT_GNSS_READY,
    DEVICE_EVENT_GNSS_FIX_ACQUIRED,
    DEVICE_EVENT_GNSS_FIX_LOST,
    DEVICE_EVENT_GNSS_FIX_UPDATE,
    DEVICE_EVENT_GNSS_STOPPED,
} device_event_id_t;

esp_err_t event_bus_init(void);
esp_err_t event_bus_post(esp_event_base_t base, int32_t id, void* data, size_t len, TickType_t timeout);

#ifdef __cplusplus
}
#endif
