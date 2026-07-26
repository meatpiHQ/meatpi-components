idf_component_register(
    SRCS "led_manager.c"
         "led_manager_settings.c"
         "led_manager_events.c"
         "led_manager_policy.c"
         "led_manager_aw2023.c"
         "led_manager_cli.c"
         "led_manager_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: owns the AW2023 RGB LED (shared bus via i2c_bus) and the
    # priority arbitration over it — highest occupied indication wins.
    # cmdline_manager only by the _cli.c (`led` command); http_server_manager
    # + esp_http_server only by the _http.c (/api/led — main wires it in HTTP
    # compositions).
    PRIV_REQUIRES event_manager i2c_bus settings_manager log_manager cmdline_manager
                  http_server_manager esp_http_server
)
