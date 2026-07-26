idf_component_register(
    SRCS "log_sinks.c"
         "log_sinks_core.c"
         "log_sinks_file.c"
         "log_sinks_settings.c"
         "log_sinks_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: the external log sinks log_manager deliberately doesn't
    # own (standard §9.3) — TCP tail server + UDP collector push (own
    # lwip sockets, the j2534 ownership model), the ws_log channel
    # (websocket_manager), and rotated files on the SD card
    # (external_storage mount-follow). Registers its sinks into
    # log_manager at start; settings gates all default false.
    PRIV_REQUIRES log_manager settings_manager websocket_manager
                  external_storage cmdline_manager lwip
)
