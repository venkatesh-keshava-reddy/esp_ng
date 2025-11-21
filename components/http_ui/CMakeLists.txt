idf_component_register(
    SRCS "http_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_server
        esp-tls
        mbedtls
        json
        config_mgr
        net_mgr
        sntp_client
        udp_broadcast
        ota_mgr
        version
        diag
)
