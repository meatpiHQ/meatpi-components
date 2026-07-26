idf_component_register(
    SRCS "bridge_manager.c"
         "bridge_manager_settings.c"
         "bridge_manager_pump.c"
         "bridge_manager_config.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Public header exposes only esp_err/FreeRTOS queue types -> all private.
    # No compile-time dep on any endpoint provider or translator (they
    # register in at runtime). cJSON via managed dep (idf_component.yml).
    PRIV_REQUIRES settings_manager log_manager
)
