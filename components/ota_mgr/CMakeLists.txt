idf_component_register(
    SRCS "ota_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_update
        esp_https_ota
        esp_http_client
        esp-tls
        config_mgr
        event_bus
        net_mgr
        version
)
