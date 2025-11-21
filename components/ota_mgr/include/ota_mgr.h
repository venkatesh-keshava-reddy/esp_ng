#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_mgr_init(void);
esp_err_t ota_mgr_trigger_from_url(const char* url);

#ifdef __cplusplus
}
#endif
