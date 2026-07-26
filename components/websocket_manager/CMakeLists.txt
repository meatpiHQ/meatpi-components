idf_component_register(
    SRCS "websocket_manager.c"
         "websocket_manager_settings.c"
         "websocket_manager_ws.c"
         "websocket_manager_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Owns WS CHANNELS, not a server: routes register into
    # http_server_manager (the one httpd; CONFIG_HTTPD_WS_SUPPORT=y in the
    # main config). Must NOT depend on bridge_manager — glue/main wraps
    # channels as endpoints. cJSON via managed dep (idf_component.yml).
    PRIV_REQUIRES http_server_manager esp_http_server settings_manager
                  log_manager
)
