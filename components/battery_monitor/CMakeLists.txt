idf_component_register(
    SRCS "battery_monitor.c"
         "battery_monitor_settings.c"
         "battery_monitor_events.c"
         "battery_monitor_policy.c"
         "battery_monitor_adc.c"
         "battery_monitor_http.c"
         "battery_monitor_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: owns the battery-sense ADC input; components register
    # threshold watches and subscribe event queues (sleep_manager's
    # foundation). /api/battery and the `battery` command are optional
    # (main wires them).
    PRIV_REQUIRES event_manager esp_adc http_server_manager
                  settings_manager log_manager cmdline_manager
)
