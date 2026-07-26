idf_component_register(
    SRCS "button_manager.c"
         "button_manager_press.c"
         "button_manager_settings.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio log_manager settings_manager
)
