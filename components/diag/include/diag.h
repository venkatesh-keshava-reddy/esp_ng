#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void diag_get_fw_version(char* out, size_t len);
uint32_t diag_get_heap_free(void);
uint64_t diag_get_uptime_s(void);
void diag_log_last_error(esp_err_t err, const char* scope);

#ifdef __cplusplus
}
#endif
