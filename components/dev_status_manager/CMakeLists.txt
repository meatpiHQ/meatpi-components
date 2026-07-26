idf_component_register(
    SRCS "dev_status_manager.c"
         "dev_status_manager_settings.c"
         "dev_status_manager_events.c"
         "dev_status_manager_fmt.c"
         "dev_status_manager_cli.c"
         "dev_status_faults.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # app_update: running partition + app descriptor (only the .c uses it);
    # cmdline_manager only by the _cli.c (version/status commands)
    PRIV_REQUIRES event_manager app_update esp_timer log_manager cmdline_manager
                  settings_manager esp_driver_tsens nvs_flash spi_flash
)
