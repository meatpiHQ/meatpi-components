idf_component_register(
    SRCS "can_manager.c"
         "can_manager_settings.c"
         "can_manager_http.c"
         "can_manager_cli.c"
         "can_isotp.c"
         "can_core.c"
         "can_core_filter.c"
         "can_core_recovery.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: owns the TWAI bus. can_core* is the adopted shared-bus
    # engine (ex elm327_can component, merged 2026-07-19 — PROVENANCE.md);
    # cmdline/http only by the surface files (§6b + §9.1).
    REQUIRES esp_driver_twai
    PRIV_REQUIRES settings_manager log_manager esp_driver_gpio
                  cmdline_manager console http_server_manager
                  esp_http_server esp_timer
)
