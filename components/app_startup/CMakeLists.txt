idf_component_register(
    SRCS "app_startup.c"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        config_store
        event_bus
        config_mgr
        wdt_mgr
        net_mgr
        provisioning_mgr
        sntp_client
        http_ui
        ota_mgr
        udp_broadcast
)
