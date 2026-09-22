# ble_http: the HTTP API over a ble_manager stream channel (loopback replay
# through esp_http_client). Builds only with the BT stack in the config, like
# ble_manager (test apps of other components must not drag BT in).
if(CONFIG_BT_ENABLED)
    set(bleh_srcs "ble_http.c"
                  "ble_http_core.c"
                  "ble_http_settings.c"
                  "ble_http_cli.c")
else()
    set(bleh_srcs "")
endif()

idf_component_register(
    SRCS ${bleh_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES ble_manager settings_manager log_manager cmdline_manager
                  console http_server_manager esp_http_client
                  espressif__cjson esp_timer
)
