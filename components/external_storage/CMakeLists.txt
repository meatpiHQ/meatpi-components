idf_component_register(
    SRCS "external_storage.c"
         "external_storage_settings.c"
         "es_detect.c"
         "external_storage_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # HAL: owns the SD device + its FAT mount at /sd. File ACCESS goes
    # through the filesystem component (main glues the mount events to it —
    # no edge between the two here, in either direction). cmdline_manager
    # only by the _cli.c (`sdcard` command).
    PRIV_REQUIRES esp_driver_sdmmc fatfs esp_driver_gpio
                  dev_status_manager log_manager cmdline_manager
                  settings_manager
)
