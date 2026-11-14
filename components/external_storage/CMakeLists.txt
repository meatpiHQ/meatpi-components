idf_component_register(
    SRCS "external_storage.c"
         "external_storage_settings.c"
         "es_detect.c"
         "es_probe.c"
         "es_close.c"
         "es_close_core.c"
         "external_storage_cli.c"
         "external_storage_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # HAL: owns the SD device + its FAT mount at /sd. File ACCESS goes
    # through the filesystem component (main glues the mount events to it:
    # no edge between the two here, in either direction). cmdline_manager
    # only by the _cli.c (`sdcard` command), http_server_manager only by
    # the _http.c (/api/sdcard), esp_timer for the format's timing.
    PRIV_REQUIRES esp_driver_sdmmc fatfs esp_driver_gpio esp_timer
                  dev_status_manager log_manager cmdline_manager
                  http_server_manager settings_manager
)

# FatFs's f_close() returns as soon as its flush fails, before it releases
# the file's lock, and IDF's VFS wipes its slot whatever the result: the
# file stays locked until the card is mounted again (a unit short of
# internal RAM gets there through the SD driver's bounce buffer). The link
# wrap puts es_close.c in front of f_close (the lwip_guard / ip4_routes
# pattern: there is no hook): a close that failed is made again without the
# flush, so the lock is released. The -u beside the wrap makes the linker
# take the wrapper out of this static library at its first scan (an
# on-target test app links every component in the tree).
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=f_close"
                      "-u __wrap_f_close")
