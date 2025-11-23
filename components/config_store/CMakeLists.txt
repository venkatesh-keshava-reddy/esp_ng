set(COMPONENT_SRCS "config_store.c")

# Include test sources when building in test mode
if(CONFIG_RUN_UNIT_TESTS)
    list(APPEND COMPONENT_SRCS "test/test_config_store.c")
endif()

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash unity
    WHOLE_ARCHIVE
)
