idf_component_register(
    SRCS "http_server_manager.c"
         "http_server_manager_match.c"
         "http_server_manager_serve.c"
         "http_server_manager_auth.c"
         "http_server_manager_settings.c"
         "http_server_manager_audit.c"
         "http_server_manager_audit_core.c"
         "http_server_manager_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # esp_http_server is REQUIRES (not PRIV): the public header exposes
    # httpd types (route tables, handles) to every registrant.
    REQUIRES esp_http_server
    # spi_flash: the flash counters of the per-route audit; cmdline_manager:
    # the `http` console command
    PRIV_REQUIRES filesystem log_manager settings_manager esp_app_format
                  spi_flash cmdline_manager
)
