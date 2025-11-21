#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t provisioning_mgr_start_if_needed(void);
esp_err_t provisioning_mgr_stop(void);

#ifdef __cplusplus
}
#endif
