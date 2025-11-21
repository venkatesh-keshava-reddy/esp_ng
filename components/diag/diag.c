#include "diag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "version.h"

void diag_get_fw_version(char* out, size_t len) {
    const char* v = version_get_string();
    if (out && len) {
        if (v) snprintf(out, len, "%s", v);
        else snprintf(out, len, "unknown");
    }
}

uint32_t diag_get_heap_free(void) {
    // NOTE: Replace with heap_caps_get_free_size(MALLOC_CAP_DEFAULT) if needed
    return esp_get_free_heap_size();
}

uint64_t diag_get_uptime_s(void) {
    return esp_timer_get_time() / 1000000ULL;
}

void diag_log_last_error(esp_err_t err, const char* scope) {
    if (err != ESP_OK) {
        ESP_LOGE("diag", "Error in %s: %s (0x%x)",
                 scope ? scope : "?", esp_err_to_name(err), err);
    }
}
