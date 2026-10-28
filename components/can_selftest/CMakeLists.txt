idf_component_register(
    SRCS "can_selftest.c"
         "can_selftest_parse.c"
         "can_selftest_settings.c"
         "can_selftest_cli.c"
         "can_selftest_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: the OBD chip and the ESP32's CAN controller exchange one OBD
    # request over the CAN pair they share (issue #1023, 2026-10-28). Above
    # both can_manager and obd_chip; main wires the routes, the settings
    # apply registers the console command (§6b).
    PRIV_REQUIRES can_manager obd_chip settings_manager log_manager
                  cmdline_manager http_server_manager esp_http_server
                  esp_timer
)
