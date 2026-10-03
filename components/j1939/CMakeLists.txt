idf_component_register(
    SRCS "j1939.c"
         "j1939_read.c"
         "j1939_tx.c"
         "j1939_settings.c"
         "j1939_http.c"
         "j1939_cli.c"
         "j1939_core.c"
         "j1939_tp_core.c"
         "j1939_dm_core.c"
         "j1939_spn_core.c"
         "j1939_cache_core.c"
         "j1939_claim_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service above can_manager: one queue subscription for every 29-bit
    # frame; transmissions only in active mode and only through
    # can_manager_send (j1939_tx.c). settings/log descriptors; the
    # /api/j1939 route and the console command only by the surface files
    # (§6b + §9.1). The *_core.c files are pure (host_test/).
    PRIV_REQUIRES settings_manager log_manager can_manager
                  cmdline_manager console http_server_manager
                  esp_http_server esp_timer
)
