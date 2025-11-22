idf_component_register(
    SRCS "udp_broadcast.c"
    INCLUDE_DIRS "include"
    REQUIRES
        lwip
        esp_timer
        config_mgr
        event_bus
        net_mgr
        diag
        version
        wdt_mgr
)
