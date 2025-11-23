#pragma once

// Stub config_store abstraction.
// Claude: implement the thin, schema-agnostic NVS helpers here.
// Goal: per-component config modules call these typed helpers instead of
// reaching into raw NVS. Keep this layer free of defaults/migrations.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize underlying storage if needed. This should be lightweight;
 * the caller still owns higher-level defaulting/migrations.
 */
esp_err_t config_store_init(void);

// Typed getters/setters. Keys are scoped by NVS namespace + key prefix.
esp_err_t config_store_get_str(const char* ns, const char* key, char* out, size_t out_len);
esp_err_t config_store_set_str(const char* ns, const char* key, const char* val);
esp_err_t config_store_get_u32(const char* ns, const char* key, uint32_t* out);
esp_err_t config_store_set_u32(const char* ns, const char* key, uint32_t val);
esp_err_t config_store_get_blob(const char* ns, const char* key, void* out, size_t* out_len);
esp_err_t config_store_set_blob(const char* ns, const char* key, const void* data, size_t data_len);
esp_err_t config_store_erase_key(const char* ns, const char* key);

// Convenience: set defaults if missing (no schema knowledge here).
esp_err_t config_store_set_if_missing_str(const char* ns, const char* key, const char* val);
esp_err_t config_store_set_if_missing_u32(const char* ns, const char* key, uint32_t val);

#ifdef __cplusplus
}
#endif
