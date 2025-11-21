/* Configuration Manager - Adapted from ESP-IDF NVS example
 * Reference: D:\esp\esp-idf\examples\storage\nvs\nvs_rw_value\
 */

#include "config_mgr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "config_mgr";
static const char *NS = "cfg";          // Runtime configuration (writable)
static const char *NS_FACTORY = "factory"; // Hardware info (readonly after provisioning)

// Schema versioning
#define CURRENT_SCHEMA_VERSION 1
#define KEY_SCHEMA_VERSION "schema/ver"

// Configuration keys
#define KEY_DEVICE_ID "sys/device_id"
#define KEY_AUTH_PASS "ui/auth_pass"

// Factory/Hardware provisioning keys (readonly after provisioning)
#define KEY_HW_MODEL "hw/model"
#define KEY_HW_REVISION "hw/revision"
#define KEY_HW_SERIAL "hw/serial"
#define KEY_GNSS_MANUFACTURER "gnss/mfr"
#define KEY_GNSS_MODEL "gnss/model"
#define KEY_GNSS_HW_VERSION "gnss/hw_ver"
#define KEY_GNSS_FW_VERSION "gnss/fw_ver"
#define KEY_PROVISION_LOCKED "prov_lock"

// Default configuration values (from IMPLEMENTATION_PLAN.md)
#define DEFAULT_UDP_ENABLED true
#define DEFAULT_UDP_ADDR "255.255.255.255"
#define DEFAULT_UDP_PORT 5005
#define DEFAULT_UDP_FREQ_HZ 1000  // Stored as millihertz (1 Hz = 1000 mHz)
#define DEFAULT_UDP_TTL 1
#define DEFAULT_UDP_MODE 0  // 0=broadcast, 1=multicast, 2=unicast

#define DEFAULT_NTRIP_PORT 2101
#define DEFAULT_NTRIP_USE_TLS false

#define DEFAULT_HTTP_UI_USER "admin"
#define DEFAULT_HTTP_UI_PASS "admin"  // WEAK - user must change

#define DEFAULT_LOG_LEVEL 3  // ESP_LOG_INFO
#define DEFAULT_NTP_SERVER "pool.ntp.org"  // Legacy - to be removed

// SNTP defaults
#define DEFAULT_SNTP_SERVER_PRIMARY "pool.ntp.org"
#define DEFAULT_SNTP_SERVER_SECONDARY "time.google.com"
#define DEFAULT_TIMEZONE "UTC0"

// Validation limits
#define MAX_STRING_LENGTH 512

/**
 * Generate device ID from MAC address
 * Format: ESP32-AABBCCDDEEFF
 */
static esp_err_t generate_device_id(char* device_id, size_t len)
{
    if (!device_id || len < 20) {  // Need at least 20 chars for "ESP32-XXXXXXXXXXXX\0"
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC for device ID: %s", esp_err_to_name(ret));
        return ret;
    }

    snprintf(device_id, len, "ESP32-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Generated device ID: %s", device_id);
    return ESP_OK;
}

/**
 * Set a default value if key doesn't exist
 * Helper to reduce code duplication in config_mgr_load_defaults_if_needed()
 */
static esp_err_t set_default_if_missing_u32(nvs_handle_t h, const char* key, uint32_t default_val)
{
    uint32_t tmp;
    esp_err_t ret = nvs_get_u32(h, key, &tmp);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = nvs_set_u32(h, key, default_val);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Set %s = %lu", key, default_val);
        }
        return ret;
    }
    // Propagate errors (corruption, etc.) or ESP_OK if read succeeded
    return ret;
}

static esp_err_t set_default_if_missing_str(nvs_handle_t h, const char* key, const char* default_val)
{
    size_t len = 0;
    esp_err_t ret = nvs_get_str(h, key, NULL, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = nvs_set_str(h, key, default_val);
        if (ret == ESP_OK) {
            // Don't log passwords
            if (strstr(key, "pass") != NULL) {
                ESP_LOGI(TAG, "Set %s = (hidden)", key);
            } else {
                ESP_LOGI(TAG, "Set %s = %s", key, default_val);
            }
        }
        return ret;
    }
    // Propagate errors (corruption, etc.) or ESP_OK if read succeeded
    return ret;
}

esp_err_t config_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing configuration manager");

    // Check and migrate schema if needed
    esp_err_t ret = config_mgr_migrate_if_needed();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Migration failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Load defaults if needed
    ret = config_mgr_load_defaults_if_needed();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load defaults: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check for weak password and warn user
    if (config_mgr_has_weak_password()) {
        ESP_LOGW(TAG, "***********************************************");
        ESP_LOGW(TAG, "* WARNING: Default password 'admin' detected! *");
        ESP_LOGW(TAG, "* Please change HTTP UI password immediately! *");
        ESP_LOGW(TAG, "***********************************************");
    }

    ESP_LOGI(TAG, "Configuration manager initialized (schema v%d)", CURRENT_SCHEMA_VERSION);
    return ESP_OK;
}

esp_err_t config_mgr_load_defaults_if_needed(void)
{
    esp_err_t ret;
    nvs_handle_t h;

    // Open NVS namespace (from NVS example pattern)
    ret = nvs_open(NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check and set schema version
    uint32_t schema_ver = 0;
    ret = nvs_get_u32(h, KEY_SCHEMA_VERSION, &schema_ver);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "First boot detected, setting schema version to %d", CURRENT_SCHEMA_VERSION);
        ret = nvs_set_u32(h, KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set schema version: %s", esp_err_to_name(ret));
            nvs_close(h);
            return ret;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read schema version: %s", esp_err_to_name(ret));
        nvs_close(h);
        return ret;
    }

    // Generate and store device ID if missing (IMPLEMENTATION_PLAN.md requirement)
    char device_id[32] = {0};
    size_t len = sizeof(device_id);
    ret = nvs_get_str(h, KEY_DEVICE_ID, device_id, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = generate_device_id(device_id, sizeof(device_id));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to generate device ID: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        ret = nvs_set_str(h, KEY_DEVICE_ID, device_id);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store device ID: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        ESP_LOGI(TAG, "Stored device ID: %s", device_id);
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device ID: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // Set UDP defaults if not present
    ret = set_default_if_missing_u32(h, "udp/enabled", DEFAULT_UDP_ENABLED ? 1 : 0);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_str(h, "udp/addr", DEFAULT_UDP_ADDR);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_u32(h, "udp/port", DEFAULT_UDP_PORT);
    if (ret != ESP_OK) goto cleanup;
    // Migrate legacy key udp/freq_hz -> udp/freq_mhz (millihertz)
    uint32_t freq_mhz = 0;
    ret = nvs_get_u32(h, "udp/freq_mhz", &freq_mhz);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Try legacy key
        if (nvs_get_u32(h, "udp/freq_hz", &freq_mhz) == ESP_OK) {
            ret = nvs_set_u32(h, "udp/freq_mhz", freq_mhz);
        } else {
            ret = set_default_if_missing_u32(h, "udp/freq_mhz", DEFAULT_UDP_FREQ_HZ);
        }
    }
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_u32(h, "udp/ttl", DEFAULT_UDP_TTL);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_u32(h, "udp/mode", DEFAULT_UDP_MODE);
    if (ret != ESP_OK) goto cleanup;

    // Set NTRIP defaults if not present
    ret = set_default_if_missing_u32(h, "ntrip/port", DEFAULT_NTRIP_PORT);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_u32(h, "ntrip/use_tls", DEFAULT_NTRIP_USE_TLS ? 1 : 0);
    if (ret != ESP_OK) goto cleanup;

    // Set HTTP UI defaults if not present
    ret = set_default_if_missing_str(h, "ui/auth_user", DEFAULT_HTTP_UI_USER);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_str(h, KEY_AUTH_PASS, DEFAULT_HTTP_UI_PASS);
    if (ret != ESP_OK) goto cleanup;

    // Set system defaults if not present
    ret = set_default_if_missing_u32(h, "sys/log_level", DEFAULT_LOG_LEVEL);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_str(h, "sys/ntp_server", DEFAULT_NTP_SERVER);  // Legacy - keeping for compatibility
    if (ret != ESP_OK) goto cleanup;

    // Set SNTP defaults if not present (NVS keys max 15 chars)
    ret = set_default_if_missing_str(h, "sntp/server1", DEFAULT_SNTP_SERVER_PRIMARY);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_str(h, "sntp/server2", DEFAULT_SNTP_SERVER_SECONDARY);
    if (ret != ESP_OK) goto cleanup;
    ret = set_default_if_missing_str(h, "sntp/timezone", DEFAULT_TIMEZONE);
    if (ret != ESP_OK) goto cleanup;

    // Commit all changes (from NVS example pattern)
    ret = nvs_commit(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit defaults: %s", esp_err_to_name(ret));
    }

cleanup:
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load defaults: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t config_mgr_get_string(const char* key, char* out, size_t out_len)
{
    // Input validation (CLAUDE_TASKS.md requirement)
    if (!key || !out || out_len == 0) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_get_string");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for key '%s': %s", key, esp_err_to_name(err));
        return err;
    }

    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to get string key '%s': %s", key, esp_err_to_name(err));
    }

    nvs_close(h);
    return err;
}

esp_err_t config_mgr_set_string(const char* key, const char* val)
{
    // Input validation (CLAUDE_TASKS.md requirement)
    if (!key || !val) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_set_string");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate string length (IMPLEMENTATION_PLAN.md requirement)
    size_t val_len = strlen(val);
    if (val_len > MAX_STRING_LENGTH) {
        ESP_LOGE(TAG, "String value too long for key '%s' (max %d bytes, got %zu)",
                 key, MAX_STRING_LENGTH, val_len);
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing key '%s': %s", key, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);  // Commit required (from NVS example)
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set string key '%s': %s", key, esp_err_to_name(err));
    }

    nvs_close(h);
    return err;
}

esp_err_t config_mgr_get_u32(const char* key, uint32_t* out)
{
    // Input validation
    if (!key || !out) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_get_u32");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u32(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t config_mgr_set_u32(const char* key, uint32_t val)
{
    // Input validation
    if (!key) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_set_u32");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing key '%s': %s", key, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);  // Commit required
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set u32 key '%s': %s", key, esp_err_to_name(err));
    }

    nvs_close(h);
    return err;
}

esp_err_t config_mgr_get_bool(const char* key, bool* out)
{
    // Input validation
    if (!key || !out) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_get_bool");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t tmp = 0;
    esp_err_t err = config_mgr_get_u32(key, &tmp);
    if (err == ESP_OK) {
        *out = (tmp != 0);
    }
    return err;
}

esp_err_t config_mgr_set_bool(const char* key, bool val)
{
    return config_mgr_set_u32(key, val ? 1u : 0u);
}

esp_err_t config_mgr_get_blob(const char* key, void* out, size_t* out_len)
{
    // Input validation
    if (!key || !out_len) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_get_blob");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for blob key '%s': %s", key, esp_err_to_name(err));
        return err;
    }

    // First call: get required size if out is NULL
    // Second call: get actual data if out is provided
    err = nvs_get_blob(h, key, out, out_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to get blob key '%s': %s", key, esp_err_to_name(err));
    }

    nvs_close(h);
    return err;
}

esp_err_t config_mgr_set_blob(const char* key, const void* data, size_t data_len)
{
    // Input validation
    if (!key || !data || data_len == 0) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_set_blob");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate blob size (NVS maximum is ~500KB per namespace, but keep reasonable)
    if (data_len > 4096) {
        ESP_LOGE(TAG, "Blob too large for key '%s' (max 4096 bytes, got %zu)",
                 key, data_len);
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing blob key '%s': %s",
                 key, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, key, data, data_len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set blob key '%s': %s", key, esp_err_to_name(err));
    }

    nvs_close(h);
    return err;
}

uint16_t config_mgr_get_schema_version(void)
{
    uint32_t ver = CURRENT_SCHEMA_VERSION;
    config_mgr_get_u32(KEY_SCHEMA_VERSION, &ver);
    return (uint16_t)ver;
}

esp_err_t config_mgr_migrate_if_needed(void)
{
    uint16_t stored_ver = config_mgr_get_schema_version();

    if (stored_ver == CURRENT_SCHEMA_VERSION) {
        // No migration needed
        return ESP_OK;
    }

    if (stored_ver < CURRENT_SCHEMA_VERSION) {
        ESP_LOGI(TAG, "Migrating schema from v%d to v%d", stored_ver, CURRENT_SCHEMA_VERSION);

        // Future migrations go here
        // Example:
        // if (stored_ver == 1 && CURRENT_SCHEMA_VERSION == 2) {
        //     // Perform v1 -> v2 migration
        //     // Add new keys, convert old formats, etc.
        // }

        // Update schema version after successful migration
        esp_err_t ret = config_mgr_set_u32(KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Migration completed successfully");
        } else {
            ESP_LOGE(TAG, "Failed to update schema version: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGW(TAG, "Stored schema v%d is newer than current v%d - proceeding with caution",
                 stored_ver, CURRENT_SCHEMA_VERSION);
    }

    return ESP_OK;
}

bool config_mgr_has_weak_password(void)
{
    char password[64] = {0};
    esp_err_t ret = config_mgr_get_string(KEY_AUTH_PASS, password, sizeof(password));

    if (ret != ESP_OK) {
        // If we can't read password, assume it's weak for safety
        return true;
    }

    // Check if password is the default "admin"
    bool is_weak = (strcmp(password, DEFAULT_HTTP_UI_PASS) == 0);

    // Clear password from memory
    memset(password, 0, sizeof(password));

    return is_weak;
}

bool config_mgr_is_provisioned(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_FACTORY, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // Factory namespace doesn't exist = not provisioned
        return false;
    }

    uint32_t locked = 0;
    err = nvs_get_u32(h, KEY_PROVISION_LOCKED, &locked);
    nvs_close(h);

    return (err == ESP_OK && locked == 1);
}

esp_err_t config_mgr_provision_hardware(const hardware_info_t* hw_info)
{
    if (!hw_info) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_provision_hardware");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate string lengths before writing (prevent partial writes + lock)
    // NVS string limit is 4000 bytes, but we enforce struct limits
    if (strnlen(hw_info->model, sizeof(hw_info->model)) >= sizeof(hw_info->model) ||
        strnlen(hw_info->revision, sizeof(hw_info->revision)) >= sizeof(hw_info->revision) ||
        strnlen(hw_info->serial, sizeof(hw_info->serial)) >= sizeof(hw_info->serial) ||
        strnlen(hw_info->gnss_manufacturer, sizeof(hw_info->gnss_manufacturer)) >= sizeof(hw_info->gnss_manufacturer) ||
        strnlen(hw_info->gnss_model, sizeof(hw_info->gnss_model)) >= sizeof(hw_info->gnss_model) ||
        strnlen(hw_info->gnss_hw_version, sizeof(hw_info->gnss_hw_version)) >= sizeof(hw_info->gnss_hw_version) ||
        strnlen(hw_info->gnss_fw_version, sizeof(hw_info->gnss_fw_version)) >= sizeof(hw_info->gnss_fw_version)) {
        ESP_LOGE(TAG, "Hardware info field exceeds maximum length (not null-terminated)");
        return ESP_ERR_INVALID_ARG;
    }

    // Check if already provisioned
    if (config_mgr_is_provisioned()) {
        ESP_LOGE(TAG, "Device already provisioned - hardware info is locked");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS_FACTORY, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open factory namespace: %s", esp_err_to_name(ret));
        return ret;
    }

    // Write hardware info atomically (all-or-nothing before lock)
    ret = nvs_set_str(h, KEY_HW_MODEL, hw_info->model);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(h, KEY_HW_REVISION, hw_info->revision);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(h, KEY_HW_SERIAL, hw_info->serial);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(h, KEY_GNSS_MANUFACTURER, hw_info->gnss_manufacturer);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(h, KEY_GNSS_MODEL, hw_info->gnss_model);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(h, KEY_GNSS_HW_VERSION, hw_info->gnss_hw_version);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(h, KEY_GNSS_FW_VERSION, hw_info->gnss_fw_version);
    if (ret != ESP_OK) goto cleanup;

    // Only lock after all writes succeed
    ret = nvs_set_u32(h, KEY_PROVISION_LOCKED, 1);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(h);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Hardware provisioned: %s rev %s, S/N: %s, GNSS: %s %s (HW:%s FW:%s)",
                 hw_info->model, hw_info->revision, hw_info->serial,
                 hw_info->gnss_manufacturer, hw_info->gnss_model,
                 hw_info->gnss_hw_version, hw_info->gnss_fw_version);
    }

cleanup:
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to provision hardware: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t config_mgr_get_hardware_info(hardware_info_t* hw_info)
{
    if (!hw_info) {
        ESP_LOGE(TAG, "Invalid arguments to config_mgr_get_hardware_info");
        return ESP_ERR_INVALID_ARG;
    }

    // Clear the structure first
    memset(hw_info, 0, sizeof(hardware_info_t));

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS_FACTORY, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Factory namespace not found (device not provisioned?)");
        return ret;
    }

    size_t len;

    len = sizeof(hw_info->model);
    ret = nvs_get_str(h, KEY_HW_MODEL, hw_info->model, &len);
    if (ret != ESP_OK) goto cleanup;

    len = sizeof(hw_info->revision);
    ret = nvs_get_str(h, KEY_HW_REVISION, hw_info->revision, &len);
    if (ret != ESP_OK) goto cleanup;

    len = sizeof(hw_info->serial);
    ret = nvs_get_str(h, KEY_HW_SERIAL, hw_info->serial, &len);
    if (ret != ESP_OK) goto cleanup;

    len = sizeof(hw_info->gnss_manufacturer);
    ret = nvs_get_str(h, KEY_GNSS_MANUFACTURER, hw_info->gnss_manufacturer, &len);
    if (ret != ESP_OK) goto cleanup;

    len = sizeof(hw_info->gnss_model);
    ret = nvs_get_str(h, KEY_GNSS_MODEL, hw_info->gnss_model, &len);
    if (ret != ESP_OK) goto cleanup;

    len = sizeof(hw_info->gnss_hw_version);
    ret = nvs_get_str(h, KEY_GNSS_HW_VERSION, hw_info->gnss_hw_version, &len);
    if (ret != ESP_OK) goto cleanup;

    len = sizeof(hw_info->gnss_fw_version);
    ret = nvs_get_str(h, KEY_GNSS_FW_VERSION, hw_info->gnss_fw_version, &len);

cleanup:
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read hardware info: %s", esp_err_to_name(ret));
    }
    return ret;
}
