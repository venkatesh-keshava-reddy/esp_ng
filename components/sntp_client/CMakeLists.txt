idf_component_register(
    SRCS "sntp_client.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_netif
        lwip
        config_mgr
        event_bus
        net_mgr
)
