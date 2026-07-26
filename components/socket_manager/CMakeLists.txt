idf_component_register(
    SRCS "socket_manager.c"
         "socket_manager_settings.c"
         "socket_manager_net.c"
         "socket_manager_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Public header exposes only esp_err/FreeRTOS queue types -> all private.
    # cJSON comes from the managed espressif/cjson dep (idf_component.yml).
    PRIV_REQUIRES lwip settings_manager log_manager esp_timer
)
