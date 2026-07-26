idf_component_register(
    SRCS "rtc_manager.c"
         "rtc_manager_settings.c"
         "rtc_manager_events.c"
         "rtc_manager_time.c"
         "rtc_manager_rx8130.c"
         "rtc_manager_http.c"
         "rtc_manager_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: owns the RX8130 battery-backed RTC (shared bus via i2c_bus);
    # restores the system clock at boot, SNTP-syncs when the network is up.
    # /api/rtc and the `rtc` command are optional (main wires them).
    PRIV_REQUIRES event_manager i2c_bus lwip esp_netif http_server_manager
                  dev_status_manager settings_manager log_manager
                  cmdline_manager
)
