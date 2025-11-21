#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hardware provisioning structure
typedef struct {
    char model[32];                 // Hardware model (e.g., "XT-600")
    char revision[16];              // Hardware revision (e.g., "1.5")
    char serial[32];                // Serial number (e.g., "XT-A1-25182012")
    char gnss_manufacturer[32];     // GNSS manufacturer (e.g., "Septentrio")
    char gnss_model[32];            // GNSS model (e.g., "Mosaic-H")
    char gnss_hw_version[16];       // GNSS hardware version (e.g., "5X")
    char gnss_fw_version[32];       // GNSS firmware version (e.g., "4.14.0")
} hardware_info_t;

esp_err_t config_mgr_init(void);
esp_err_t config_mgr_load_defaults_if_needed(void);

esp_err_t config_mgr_get_string(const char* key, char* out, size_t out_len);
esp_err_t config_mgr_set_string(const char* key, const char* val);

esp_err_t config_mgr_get_u32(const char* key, uint32_t* out);
esp_err_t config_mgr_set_u32(const char* key, uint32_t val);

esp_err_t config_mgr_get_bool(const char* key, bool* out);
esp_err_t config_mgr_set_bool(const char* key, bool val);

// Blob storage (for large data like certificates)
esp_err_t config_mgr_get_blob(const char* key, void* out, size_t* out_len);
esp_err_t config_mgr_set_blob(const char* key, const void* data, size_t data_len);

uint16_t config_mgr_get_schema_version(void);
esp_err_t config_mgr_migrate_if_needed(void);

// Security helpers
bool config_mgr_has_weak_password(void);

// Hardware provisioning API
bool config_mgr_is_provisioned(void);
esp_err_t config_mgr_provision_hardware(const hardware_info_t* hw_info);
esp_err_t config_mgr_get_hardware_info(hardware_info_t* hw_info);

#ifdef __cplusplus
}
#endif
