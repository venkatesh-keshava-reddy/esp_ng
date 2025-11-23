# Pending Issues for test-harness Branch

This document tracks issues identified during PR review that need to be addressed before merging.

---

## Issue #1: Missing Headers for Restart Delay Logic

**Source**: PR #3 Review Comment
**Severity**: P1 - Compilation Failure
**Status**: Pending Fix

### Problem

The test-harness path in `main/main.c` (lines 36-39) calls:
- `vTaskDelay(pdMS_TO_TICKS(1000))`
- `esp_restart()`

However, the required headers are not included:
- `freertos/FreeRTOS.h` and `freertos/task.h` for `vTaskDelay()` and `pdMS_TO_TICKS()`
- `esp_system.h` for `esp_restart()`

This causes compilation failure under ESP-IDF's default `-Werror=implicit-function-declaration`.

### Current Code (main/main.c:1-15)
```c
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"

// Component headers
#include "version.h"
#include "app_startup.h"

#ifdef CONFIG_RUN_UNIT_TESTS
#include "test_harness.h"
#endif
```

### Proposed Fix

Add the missing headers at the top of `main/main.c`:

```c
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"           // ADD: for esp_restart()
#include "freertos/FreeRTOS.h"    // ADD: for FreeRTOS types
#include "freertos/task.h"        // ADD: for vTaskDelay()

// Component headers
#include "version.h"
#include "app_startup.h"

#ifdef CONFIG_RUN_UNIT_TESTS
#include "test_harness.h"
#endif
```

### Files to Modify
- `main/main.c` - Add three missing headers

---

## Issue #2: Unit Test Mode Skips NVS Initialization

**Source**: PR #2 Review Comment (still applicable to PR #3)
**Severity**: P1 - Test Failure
**Status**: Pending Fix

### Problem

When `CONFIG_RUN_UNIT_TESTS` is enabled, `app_main()` runs the test harness and then reboots, never reaching `app_startup_run_generic()` which performs NVS/config_store initialization.

The test path currently initializes (main/main.c:29-31):
- `esp_netif_init()`
- `esp_event_loop_create_default()`

**But it's missing**:
- NVS initialization

This causes Unity tests that touch NVS/config_store to fail with `ESP_ERR_NVS_NOT_INITIALIZED` before any test logic runs.

### Current Code (main/main.c:24-40)
```c
#ifdef CONFIG_RUN_UNIT_TESTS
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "           UNIT TEST MODE");
    ESP_LOGI(TAG, "========================================");

    // Initialize minimal services needed by tests
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Run component test harness with component grouping
    test_harness_run();

    // Reboot to re-enter test menu (no app startup in test builds)
    ESP_LOGI(TAG, "Test harness exited. Rebooting into test menu...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Brief delay to see the message
    esp_restart();
#endif
```

### Proposed Fix

**Option 1: Direct NVS Initialization (Recommended)**

Add NVS initialization directly in the test path:

```c
#ifdef CONFIG_RUN_UNIT_TESTS
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "           UNIT TEST MODE");
    ESP_LOGI(TAG, "========================================");

    // Initialize minimal services needed by tests
    ESP_ERROR_CHECK(nvs_flash_init());              // ADD: Initialize NVS
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Run component test harness with component grouping
    test_harness_run();

    // Reboot to re-enter test menu (no app startup in test builds)
    ESP_LOGI(TAG, "Test harness exited. Rebooting into test menu...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Brief delay to see the message
    esp_restart();
#endif
```

**Rationale for Option 1**:
- Tests should be able to test `config_store_init()` itself
- Keeps test environment minimal and explicit
- Matches the existing comment "minimal services needed by tests"
- NVS is a fundamental dependency, like networking services

**Option 2: Use config_store_init() (Alternative)**

```c
#ifdef CONFIG_RUN_UNIT_TESTS
    // Initialize minimal services needed by tests
    ESP_ERROR_CHECK(config_store_init());           // ADD: Initialize config_store (wraps NVS)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ... rest of code
#endif
```

**Drawback of Option 2**:
- Makes it harder to test `config_store_init()` edge cases
- Tests would always start with config_store already initialized

### Files to Modify
- `main/main.c` - Add NVS initialization (need to add `#include "nvs_flash.h"`)

### Additional Header Required

For Option 1, add to the includes:
```c
#include "nvs_flash.h"    // ADD: for nvs_flash_init()
```

For Option 2, add to the includes:
```c
#include "config_store.h"  // ADD: for config_store_init()
```

---

## Summary

Both issues are **P1 blockers** that prevent:
1. **Issue #1**: Code from compiling in test mode
2. **Issue #2**: Tests from running successfully (NVS-dependent tests will fail)

### Recommended Fix Order
1. Fix Issue #1 first (missing headers) - enables compilation
2. Fix Issue #2 second (NVS init) - enables tests to pass

### Complete Set of Changes Required

**main/main.c header section:**
```c
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"           // ADD for Issue #1
#include "freertos/FreeRTOS.h"    // ADD for Issue #1
#include "freertos/task.h"        // ADD for Issue #1
#include "nvs_flash.h"            // ADD for Issue #2

// Component headers
#include "version.h"
#include "app_startup.h"

#ifdef CONFIG_RUN_UNIT_TESTS
#include "test_harness.h"
#endif
```

**main/main.c test initialization section:**
```c
#ifdef CONFIG_RUN_UNIT_TESTS
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "           UNIT TEST MODE");
    ESP_LOGI(TAG, "========================================");

    // Initialize minimal services needed by tests
    ESP_ERROR_CHECK(nvs_flash_init());              // ADD for Issue #2
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Run component test harness with component grouping
    test_harness_run();

    // Reboot to re-enter test menu (no app startup in test builds)
    ESP_LOGI(TAG, "Test harness exited. Rebooting into test menu...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Brief delay to see the message
    esp_restart();
#endif
```

---

**Document Version**: 1.0
**Last Updated**: 2025-11-23
**Branch**: feature/test-harness
**PR**: #3
