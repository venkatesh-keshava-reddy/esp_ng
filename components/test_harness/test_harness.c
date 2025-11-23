#include "test_harness.h"
#include "unity.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "test_harness";

// Component test tags - add new components here
typedef struct {
    const char *name;
    const char *tag;
} component_info_t;

static const component_info_t components[] = {
    {"Config Store", "config_store"},
    // Add more components as tests are created:
    // {"Event Bus", "event_bus"},
    // {"Network Manager", "net_mgr"},
};

static const size_t num_components = sizeof(components) / sizeof(components[0]);

/**
 * @brief Flush any remaining characters from stdin
 *
 * Prevents buffered input from carrying over to the next menu prompt.
 */
static void flush_stdin(void)
{
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        // Consume characters until newline or EOF
    }
}

/**
 * @brief Run all tests for a specific component tag
 *
 * Note: unity_run_tests_by_tag() doesn't provide a way to detect if tests were found,
 * so we just run the tagged tests. If your component tests aren't tagged, they won't run.
 * Ensure all component tests use tags like: TEST_CASE("[config_store] test", "[config_store]")
 */
static void run_component_tests(const char *tag)
{
    printf("\n");
    printf("========================================\n");
    printf("Running tests for [%s]\n", tag);
    printf("========================================\n");

    UNITY_BEGIN();
    unity_run_tests_by_tag(tag, false);
    UNITY_END();
}

/**
 * @brief Run all tests across all components
 */
static void run_all_tests(void)
{
    printf("\n");
    printf("========================================\n");
    printf("Running ALL tests\n");
    printf("========================================\n");

    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}

/**
 * @brief Display component menu and handle selection
 */
static void show_component_menu(void)
{
    while (1) {
        printf("\n");
        printf("========================================\n");
        printf("        Component Test Menu\n");
        printf("========================================\n");
        printf("\n");
        printf("  0. Run ALL Tests (All Components)\n");
        printf("\n");

        for (size_t i = 0; i < num_components; i++) {
            printf("  %zu. %s\n", i + 1, components[i].name);
        }

        printf("\n");
        printf("  q. Quit\n");
        printf("\n");
        printf("Select component: ");
        fflush(stdout);

        char input[16];
        memset(input, 0, sizeof(input));

        // Read input character by character until newline
        size_t idx = 0;
        while (idx < sizeof(input) - 1) {
            int c = fgetc(stdin);
            if (c == EOF) {
                // No input available, wait a bit
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (c == '\r' || c == '\n') {
                printf("\n");  // Echo newline
                break;
            }
            // Echo the character
            printf("%c", c);
            fflush(stdout);
            input[idx++] = (char)c;
        }

        input[idx] = '\0';

        // Flush any remaining buffered input
        if (idx >= sizeof(input) - 1) {
            flush_stdin();
        }

        // Skip if empty input
        if (idx == 0) {
            continue;
        }

        // Handle quit
        if (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0) {
            printf("\nExiting test harness.\n");
            return;
        }

        // Parse selection
        int selection = atoi(input);

        if (selection == 0) {
            run_all_tests();
        } else if (selection > 0 && selection <= (int)num_components) {
            const component_info_t *comp = &components[selection - 1];
            run_component_tests(comp->tag);
        } else {
            printf("\nInvalid selection. Please try again.\n");
        }
    }
}

void test_harness_run(void)
{
    ESP_LOGI(TAG, "Starting component test harness");

    printf("\n");
    printf("========================================\n");
    printf("      ESP-NG Component Test Menu\n");
    printf("========================================\n");
    printf("\n");
    printf("Total components with tests: %zu\n", num_components);

    show_component_menu();
}
