idf_component_register(
    SRCS "diag.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        version
)
