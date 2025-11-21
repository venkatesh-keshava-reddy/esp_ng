idf_component_register(
    SRCS "wdt_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        event_bus
)
