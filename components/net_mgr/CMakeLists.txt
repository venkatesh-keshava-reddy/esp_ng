idf_component_register(
    SRCS "net_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        lwip
        config_mgr
        event_bus
)
