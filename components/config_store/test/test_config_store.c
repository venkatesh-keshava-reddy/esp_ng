#include "unity.h"
#include "config_store.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include <string.h>

// Test namespace and keys
#define TEST_NS "test_ns"
#define TEST_KEY "test_key"

void setUp(void)
{
    // Initialize config_store before each test
    TEST_ASSERT_EQUAL(ESP_OK, config_store_init());
}

void tearDown(void)
{
    // Clean up test data after each test
    config_store_erase_key(TEST_NS, TEST_KEY);
}

TEST_CASE("config_store_init_succeeds", "[config_store]")
{
    // Init should be idempotent
    TEST_ASSERT_EQUAL(ESP_OK, config_store_init());
    TEST_ASSERT_EQUAL(ESP_OK, config_store_init());
}

TEST_CASE("config_store_str_roundtrip", "[config_store]")
{
    const char* test_val = "hello_world";
    char read_buf[32];

    // Key should not exist initially
    esp_err_t ret = config_store_get_str(TEST_NS, TEST_KEY, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);

    // Write string
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_str(TEST_NS, TEST_KEY, test_val));

    // Read back
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_str(TEST_NS, TEST_KEY, read_buf, sizeof(read_buf)));
    TEST_ASSERT_EQUAL_STRING(test_val, read_buf);

    // Erase
    TEST_ASSERT_EQUAL(ESP_OK, config_store_erase_key(TEST_NS, TEST_KEY));

    // Verify erased
    ret = config_store_get_str(TEST_NS, TEST_KEY, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);
}

TEST_CASE("config_store_str_buffer_too_small", "[config_store]")
{
    const char* test_val = "this_is_a_long_string_value";
    char small_buf[5];

    // Write string
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_str(TEST_NS, TEST_KEY, test_val));

    // Try to read into too-small buffer
    esp_err_t ret = config_store_get_str(TEST_NS, TEST_KEY, small_buf, sizeof(small_buf));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_INVALID_LENGTH, ret);

    // Should work with larger buffer
    char large_buf[64];
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_str(TEST_NS, TEST_KEY, large_buf, sizeof(large_buf)));
    TEST_ASSERT_EQUAL_STRING(test_val, large_buf);
}

TEST_CASE("config_store_u32_roundtrip", "[config_store]")
{
    uint32_t test_val = 0x12345678;
    uint32_t read_val = 0;

    // Key should not exist initially
    esp_err_t ret = config_store_get_u32(TEST_NS, TEST_KEY, &read_val);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);

    // Write u32
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_u32(TEST_NS, TEST_KEY, test_val));

    // Read back
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_u32(TEST_NS, TEST_KEY, &read_val));
    TEST_ASSERT_EQUAL_UINT32(test_val, read_val);

    // Update value
    uint32_t new_val = 0xABCDEF00;
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_u32(TEST_NS, TEST_KEY, new_val));
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_u32(TEST_NS, TEST_KEY, &read_val));
    TEST_ASSERT_EQUAL_UINT32(new_val, read_val);

    // Erase
    TEST_ASSERT_EQUAL(ESP_OK, config_store_erase_key(TEST_NS, TEST_KEY));
    ret = config_store_get_u32(TEST_NS, TEST_KEY, &read_val);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);
}

TEST_CASE("config_store_blob_roundtrip", "[config_store]")
{
    uint8_t test_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    uint8_t read_buf[16];
    size_t read_len = sizeof(read_buf);

    // Key should not exist initially
    esp_err_t ret = config_store_get_blob(TEST_NS, TEST_KEY, read_buf, &read_len);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);

    // Write blob
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_blob(TEST_NS, TEST_KEY, test_data, sizeof(test_data)));

    // Read back
    read_len = sizeof(read_buf);
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_blob(TEST_NS, TEST_KEY, read_buf, &read_len));
    TEST_ASSERT_EQUAL_size_t(sizeof(test_data), read_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(test_data, read_buf, sizeof(test_data));

    // Erase
    TEST_ASSERT_EQUAL(ESP_OK, config_store_erase_key(TEST_NS, TEST_KEY));
    read_len = sizeof(read_buf);
    ret = config_store_get_blob(TEST_NS, TEST_KEY, read_buf, &read_len);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);
}

TEST_CASE("config_store_blob_buffer_too_small", "[config_store]")
{
    uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t small_buf[4];
    size_t small_len = sizeof(small_buf);

    // Write blob
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_blob(TEST_NS, TEST_KEY, test_data, sizeof(test_data)));

    // Try to read into too-small buffer
    esp_err_t ret = config_store_get_blob(TEST_NS, TEST_KEY, small_buf, &small_len);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_INVALID_LENGTH, ret);

    // Should work with larger buffer
    uint8_t large_buf[16];
    size_t large_len = sizeof(large_buf);
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_blob(TEST_NS, TEST_KEY, large_buf, &large_len));
    TEST_ASSERT_EQUAL_size_t(sizeof(test_data), large_len);
}

TEST_CASE("config_store_set_if_missing_str", "[config_store]")
{
    const char* val1 = "first_value";
    const char* val2 = "second_value";
    char read_buf[32];

    // First set should succeed
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_if_missing_str(TEST_NS, TEST_KEY, val1));

    // Verify it was set
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_str(TEST_NS, TEST_KEY, read_buf, sizeof(read_buf)));
    TEST_ASSERT_EQUAL_STRING(val1, read_buf);

    // Second set should skip (key exists)
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_if_missing_str(TEST_NS, TEST_KEY, val2));

    // Value should still be val1
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_str(TEST_NS, TEST_KEY, read_buf, sizeof(read_buf)));
    TEST_ASSERT_EQUAL_STRING(val1, read_buf);
}

TEST_CASE("config_store_set_if_missing_u32", "[config_store]")
{
    uint32_t val1 = 0x11111111;
    uint32_t val2 = 0x22222222;
    uint32_t read_val = 0;

    // First set should succeed
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_if_missing_u32(TEST_NS, TEST_KEY, val1));

    // Verify it was set
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_u32(TEST_NS, TEST_KEY, &read_val));
    TEST_ASSERT_EQUAL_UINT32(val1, read_val);

    // Second set should skip (key exists)
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_if_missing_u32(TEST_NS, TEST_KEY, val2));

    // Value should still be val1
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_u32(TEST_NS, TEST_KEY, &read_val));
    TEST_ASSERT_EQUAL_UINT32(val1, read_val);
}

TEST_CASE("config_store_namespace_validation", "[config_store]")
{
    char buf[32];

    // Namespace too long (>15 chars)
    const char* long_ns = "this_namespace_is_way_too_long";
    esp_err_t ret = config_store_get_str(long_ns, TEST_KEY, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);

    ret = config_store_set_str(long_ns, TEST_KEY, "value");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);

    // NULL namespace
    ret = config_store_get_str(NULL, TEST_KEY, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

TEST_CASE("config_store_key_validation", "[config_store]")
{
    char buf[32];

    // Key too long (>15 chars)
    const char* long_key = "this_key_is_way_too_long_for_nvs";
    esp_err_t ret = config_store_get_str(TEST_NS, long_key, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);

    ret = config_store_set_str(TEST_NS, long_key, "value");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);

    // NULL key
    ret = config_store_get_str(TEST_NS, NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

TEST_CASE("config_store_erase_nonexistent_key", "[config_store]")
{
    // Erasing non-existent key should return ESP_ERR_NVS_NOT_FOUND
    esp_err_t ret = config_store_erase_key(TEST_NS, "nonexistent_key");
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);

    // Erasing twice should both return NOT_FOUND
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_str(TEST_NS, TEST_KEY, "value"));
    TEST_ASSERT_EQUAL(ESP_OK, config_store_erase_key(TEST_NS, TEST_KEY));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, config_store_erase_key(TEST_NS, TEST_KEY));
}

TEST_CASE("config_store_null_termination", "[config_store]")
{
    const char* test_val = "test";
    char read_buf[10];

    // Fill buffer with non-zero values
    memset(read_buf, 0xFF, sizeof(read_buf));

    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_str(TEST_NS, TEST_KEY, test_val));
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get_str(TEST_NS, TEST_KEY, read_buf, sizeof(read_buf)));

    // Verify null-terminated
    TEST_ASSERT_EQUAL_STRING(test_val, read_buf);
    TEST_ASSERT_EQUAL(0, read_buf[strlen(test_val)]);
}
