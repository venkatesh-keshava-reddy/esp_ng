/* config_store - Thin NVS abstraction layer
 *
 * Schema-agnostic typed helpers for NVS storage.
 * Per-component config modules call these instead of raw NVS.
 * This layer is free of defaults/migrations (that's config_mgr's job).
 */

#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_store";

// NVS limits per ESP-IDF documentation
#define NVS_MAX_NAMESPACE_LEN 15
#define NVS_MAX_KEY_LEN 15

/**
 * Initialize NVS flash storage
 * Handles the no-free-pages case as per ESP-IDF examples
 */
esp_err_t config_store_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated or is in new format
        // Erase and retry initialization
        ESP_LOGW(TAG, "NVS flash init failed (%s), erasing and retrying", esp_err_to_name(ret));

        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS flash erase failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = nvs_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS flash init failed after erase: %s", esp_err_to_name(ret));
            return ret;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "NVS flash initialized successfully");
    return ESP_OK;
}

/**
 * Validate namespace and key lengths against NVS limits
 * Returns ESP_ERR_INVALID_ARG if either exceeds limits
 */
static esp_err_t validate_ns_key(const char* ns, const char* key)
{
    if (!ns || !key) {
        ESP_LOGE(TAG, "Namespace or key is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    size_t ns_len = strlen(ns);
    size_t key_len = strlen(key);

    if (ns_len == 0 || ns_len > NVS_MAX_NAMESPACE_LEN) {
        ESP_LOGE(TAG, "Namespace '%s' length %zu exceeds limit %d", ns, ns_len, NVS_MAX_NAMESPACE_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    if (key_len == 0 || key_len > NVS_MAX_KEY_LEN) {
        ESP_LOGE(TAG, "Key '%s' length %zu exceeds limit %d", key, key_len, NVS_MAX_KEY_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * Get string from NVS
 * On success, output is null-terminated
 * Returns ESP_ERR_NVS_NOT_FOUND if key doesn't exist
 * Returns ESP_ERR_NVS_INVALID_LENGTH if stored value won't fit in out_len
 */
esp_err_t config_store_get_str(const char* ns, const char* key, char* out, size_t out_len)
{
    if (!out || out_len == 0) {
        ESP_LOGE(TAG, "Invalid output buffer");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = validate_ns_key(ns, key);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(ns, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    // Get required size first
    size_t required_size = 0;
    ret = nvs_get_str(handle, key, NULL, &required_size);
    if (ret != ESP_OK) {
        nvs_close(handle);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "Key '%s/%s' not found", ns, key);
        } else {
            ESP_LOGE(TAG, "nvs_get_str(%s/%s) size query failed: %s", ns, key, esp_err_to_name(ret));
        }
        return ret;
    }

    // Check if buffer is large enough (including null terminator)
    if (required_size > out_len) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Buffer too small for key '%s/%s': need %zu, have %zu",
                 ns, key, required_size, out_len);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    // Read the string
    ret = nvs_get_str(handle, key, out, &out_len);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(%s/%s) read failed: %s", ns, key, esp_err_to_name(ret));
        return ret;
    }

    // String is already null-terminated by nvs_get_str
    ESP_LOGD(TAG, "Read string '%s/%s': '%s'", ns, key, out);
    return ESP_OK;
}

/**
 * Set string in NVS
 * Value must be null-terminated
 */
esp_err_t config_store_set_str(const char* ns, const char* key, const char* val)
{
    if (!val) {
        ESP_LOGE(TAG, "Value is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = validate_ns_key(ns, key);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(ns, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, key, val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s/%s) failed: %s", ns, key, esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Wrote string '%s/%s': '%s'", ns, key, val);
    return ESP_OK;
}

/**
 * Get u32 from NVS
 * Returns ESP_ERR_NVS_NOT_FOUND if key doesn't exist
 */
esp_err_t config_store_get_u32(const char* ns, const char* key, uint32_t* out)
{
    if (!out) {
        ESP_LOGE(TAG, "Output pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = validate_ns_key(ns, key);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(ns, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_get_u32(handle, key, out);
    nvs_close(handle);

    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "Key '%s/%s' not found", ns, key);
        } else {
            ESP_LOGE(TAG, "nvs_get_u32(%s/%s) failed: %s", ns, key, esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGD(TAG, "Read u32 '%s/%s': %lu", ns, key, (unsigned long)*out);
    return ESP_OK;
}

/**
 * Set u32 in NVS
 */
esp_err_t config_store_set_u32(const char* ns, const char* key, uint32_t val)
{
    esp_err_t ret = validate_ns_key(ns, key);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(ns, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u32(handle, key, val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32(%s/%s) failed: %s", ns, key, esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Wrote u32 '%s/%s': %lu", ns, key, (unsigned long)val);
    return ESP_OK;
}

/**
 * Get blob from NVS
 * Mirrors NVS semantics exactly:
 * - If out == NULL, returns required size in *out_len (size query)
 * - If out != NULL and buffer too small, returns ESP_ERR_NVS_INVALID_LENGTH
 *   and writes required size to *out_len
 * - On success, *out_len is updated with actual blob size
 * Returns ESP_ERR_NVS_NOT_FOUND if key doesn't exist
 */
esp_err_t config_store_get_blob(const char* ns, const char* key, void* out, size_t* out_len)
{
    if (!out_len) {
        ESP_LOGE(TAG, "Length pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = validate_ns_key(ns, key);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(ns, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    // Get required size first
    size_t required_size = 0;
    ret = nvs_get_blob(handle, key, NULL, &required_size);
    if (ret != ESP_OK) {
        nvs_close(handle);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "Key '%s/%s' not found", ns, key);
        } else {
            ESP_LOGE(TAG, "nvs_get_blob(%s/%s) size query failed: %s", ns, key, esp_err_to_name(ret));
        }
        return ret;
    }

    // If out == NULL, this is a size query only
    if (out == NULL) {
        nvs_close(handle);
        *out_len = required_size;
        ESP_LOGD(TAG, "Size query for '%s/%s': %zu bytes", ns, key, required_size);
        return ESP_OK;
    }

    // Check if buffer is large enough
    if (required_size > *out_len) {
        nvs_close(handle);
        // Mirror NVS semantics: write required size to *out_len before returning error
        *out_len = required_size;
        ESP_LOGE(TAG, "Buffer too small for key '%s/%s': need %zu, have %zu",
                 ns, key, required_size, *out_len);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    // Read the blob
    size_t actual_len = *out_len;
    ret = nvs_get_blob(handle, key, out, &actual_len);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(%s/%s) read failed: %s", ns, key, esp_err_to_name(ret));
        return ret;
    }

    *out_len = actual_len;
    ESP_LOGD(TAG, "Read blob '%s/%s': %zu bytes", ns, key, actual_len);
    return ESP_OK;
}

/**
 * Set blob in NVS
 */
esp_err_t config_store_set_blob(const char* ns, const char* key, const void* data, size_t data_len)
{
    if (!data) {
        ESP_LOGE(TAG, "Data pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = validate_ns_key(ns, key);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(ns, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, key, data, data_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(%s/%s) failed: %s", ns, key, esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Wrote blob '%s/%s': %zu bytes", ns, key, data_len);
    return ESP_OK;
}

/**
 * Erase a key from NVS
 * Returns ESP_ERR_NVS_NOT_FOUND if key doesn't exist (not an error)
 */
esp_err_t config_store_erase_key(const char* ns, const char* key)
{
    esp_err_t ret = validate_ns_key(ns, key);
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t handle;
    ret = nvs_open(ns, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_erase_key(handle, key);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_erase_key(%s/%s) failed: %s", ns, key, esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    esp_err_t commit_ret = nvs_commit(handle);
    nvs_close(handle);

    if (commit_ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(%s) failed: %s", ns, esp_err_to_name(commit_ret));
        return commit_ret;
    }

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "Key '%s/%s' already absent", ns, key);
    } else {
        ESP_LOGD(TAG, "Erased key '%s/%s'", ns, key);
    }

    return ret;  // Return ESP_ERR_NVS_NOT_FOUND if key was already missing
}

/**
 * Set string only if key doesn't exist
 * Returns ESP_OK if value was set or already exists
 */
esp_err_t config_store_set_if_missing_str(const char* ns, const char* key, const char* val)
{
    // Check if key exists
    char dummy[1];
    esp_err_t ret = config_store_get_str(ns, key, dummy, sizeof(dummy));

    if (ret == ESP_OK || ret == ESP_ERR_NVS_INVALID_LENGTH) {
        // Key exists (either fits in dummy buffer or is too large)
        ESP_LOGD(TAG, "Key '%s/%s' already exists, skipping set", ns, key);
        return ESP_OK;
    }

    if (ret != ESP_ERR_NVS_NOT_FOUND) {
        // Some other error occurred
        return ret;
    }

    // Key doesn't exist, set it
    return config_store_set_str(ns, key, val);
}

/**
 * Set u32 only if key doesn't exist
 * Returns ESP_OK if value was set or already exists
 */
esp_err_t config_store_set_if_missing_u32(const char* ns, const char* key, uint32_t val)
{
    // Check if key exists
    uint32_t dummy;
    esp_err_t ret = config_store_get_u32(ns, key, &dummy);

    if (ret == ESP_OK) {
        // Key exists
        ESP_LOGD(TAG, "Key '%s/%s' already exists, skipping set", ns, key);
        return ESP_OK;
    }

    if (ret != ESP_ERR_NVS_NOT_FOUND) {
        // Some other error occurred
        return ret;
    }

    // Key doesn't exist, set it
    return config_store_set_u32(ns, key, val);
}
