#pragma once
#include "esp_err.h"
#include <stdint.h>

typedef enum { UDP_MODE_BROADCAST, UDP_MODE_MULTICAST, UDP_MODE_UNICAST } udp_mode_t;

typedef struct {
    udp_mode_t mode;
    char addr[48];
    uint16_t port;
    uint16_t freq_mhz;  // Frequency in millihertz (1 Hz = 1000 mHz); supports 0.2 Hz to 5 Hz
    uint8_t ttl;
} udp_cfg_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t udp_broadcast_start(void);
esp_err_t udp_broadcast_stop(void);
esp_err_t udp_broadcast_apply_config(const udp_cfg_t* cfg);
esp_err_t udp_broadcast_publish_now(void);

#ifdef __cplusplus
}
#endif
