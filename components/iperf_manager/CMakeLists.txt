idf_component_register(
    SRCS "iperf_manager.c"
         "iperf_manager_settings.c"
         "iperf_manager_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: on-demand link-throughput measurement (the managed
    # espressif/iperf engine, iperf2-compatible) driven by the `iperf`
    # console command — WiFi/USB-NCM/ESPNetlink legs against a PC or
    # the bench Pi.
    PRIV_REQUIRES settings_manager log_manager cmdline_manager
)
