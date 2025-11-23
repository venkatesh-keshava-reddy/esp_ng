#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run interactive component test menu
 *
 * Displays a menu for selecting component test suites:
 * - Select a component to run all tests tagged with that component
 * - Select "Run All Tests" to execute all tests across all components
 * - Tests must be tagged (e.g., TEST_CASE("test", "[config_store]")) to appear in component menus
 */
void test_harness_run(void);

#ifdef __cplusplus
}
#endif
