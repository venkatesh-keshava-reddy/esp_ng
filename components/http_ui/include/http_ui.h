#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_ui_start(void);
esp_err_t http_ui_stop(void);
esp_err_t http_ui_update_auth(const char* user, const char* pass);

#ifdef __cplusplus
}
#endif
