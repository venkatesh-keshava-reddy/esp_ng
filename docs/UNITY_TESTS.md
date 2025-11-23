# How to run Unity tests

This project supports two builds:

- **Normal build**: `CONFIG_RUN_UNIT_TESTS` disabled (default) - normal app starts.
- **Test build**: `CONFIG_RUN_UNIT_TESTS` enabled - test harness menu runs instead of the app.

## Config toggle

- In `menuconfig`: Application Configuration -> Enable Unity Test Menu (`CONFIG_RUN_UNIT_TESTS`).
- When enabled, `main.c` calls `test_harness_run()`. After quitting the harness, we will reboot into the menu (see implementation notes).

## Build commands

- **Test build (recommended separate dir):**
  ```
  idf.py -B build_tests -DTEST_COMPONENTS=config_store -DCONFIG_RUN_UNIT_TESTS=y build
  idf.py -p COM3 -B build_tests -DTEST_COMPONENTS=config_store -DCONFIG_RUN_UNIT_TESTS=y flash monitor
  ```
  Add `-p <PORT>` / `-b <BAUD>` as needed.
- **Normal build (no tests):**
  ```
  idf.py -B build_normal build flash monitor
  ```
  Leave `CONFIG_RUN_UNIT_TESTS` off (default) or omit any `-D` overrides.

## Using the test harness

- On boot (test build), you'll see the component menu.
- `0` runs all tests; selecting a component runs tests tagged with that component (e.g., `[config_store]`).
- `q` quits; we will reboot into the menu (no app startup in test builds).
- Untagged tests are not run via the component menu; use "Run ALL" to execute everything.

## Implementation notes for Claude

- **Flag naming**: Standardize on `-DCONFIG_RUN_UNIT_TESTS=y` everywhere; remove `-DRUN_UNIT_TESTS=1` from any docs.
- **CMake linking**: `test_harness` is always in `PRIV_REQUIRES` because conditional linking doesn't work reliably with ESP-IDF's Kconfig/CMake timing. The code in `test_harness` is only compiled/used when `CONFIG_RUN_UNIT_TESTS` is enabled via `#ifdef` guards, and the linker strips unused code from final binaries.
- **Post-quit behavior**: After `test_harness_run()`, call `esp_restart()` so quitting reboots into the menu. Do not fall through to the app in test builds.
- **Docs**: Delete `test_harness/README.md` and point to `docs/UNITY_TESTS.md` as the single source.
- **Harness cleanup**: Keep output ASCII with a consistent 40-char `=` separator; flush stdin after each menu interaction to avoid buffered carry-over.
- **Component registration**: Manual array editing is fine for now; no auto-discovery needed.
- **Tags**: Ensure component tests are tagged (e.g., `[config_store]`); otherwise, "Run ALL" is the only way to run untagged tests.
- **Kconfig wording**: Keep `CONFIG_RUN_UNIT_TESTS` as the symbol; description can remain "Enable Unity Test Menu".

## Notes

- `TEST_COMPONENTS` scopes which tests are built; omit to build all.
- Tests run on target; keep the test harness enabled only for development builds.
