# The crash note's hooks walk an Xtensa stack and keep the note in RTC
# memory: the WiCAN Pro (ESP32-S3). Any other target builds the stub.
if(CONFIG_IDF_TARGET_ARCH_XTENSA AND CONFIG_SOC_RTC_MEM_SUPPORTED)
    set(crash_src "restart_tracker_crash.c")
else()
    set(crash_src "restart_tracker_crash_stub.c")
endif()

idf_component_register(
    SRCS "restart_tracker.c"
         "restart_tracker_core.c"
         "restart_tracker_crash_core.c"
         "restart_tracker_brake_core.c"
         "restart_tracker_report_core.c"
         "restart_tracker_report.c"
         "${crash_src}"
         "restart_tracker_cli.c"
         "restart_tracker_settings.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # cache msync + esp_timer are only used by the .c files;
    # cmdline_manager only by the _cli.c (`restart_tracker` command);
    # esp_app_format: the image id the crash note carries;
    # nvs_flash: the stored crash report (restart_tracker_report.c)
    PRIV_REQUIRES esp_mm esp_timer log_manager cmdline_manager
                  settings_manager esp_app_format nvs_flash
)

if(CONFIG_IDF_TARGET_ARCH_XTENSA AND CONFIG_SOC_RTC_MEM_SUPPORTED)
    # The crash note: restart_tracker_crash.c wraps IDF's panic entry and
    # exit (the form can_manager uses for twai_hal_read_rx_fifo). Both
    # symbols are private to IDF: run `.\test.ps1 crashnote` and the target
    # app at every IDF version bump.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
                          "-Wl,--wrap=esp_panic_handler"
                          "-Wl,--wrap=panic_restart")
endif()
