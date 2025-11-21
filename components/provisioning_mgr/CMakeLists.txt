idf_component_register(
    SRCS "provisioning_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES
        wifi_provisioning
        esp_event
        esp_netif
        config_mgr
        event_bus
        net_mgr
)
