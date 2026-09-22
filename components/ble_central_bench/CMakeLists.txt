# ble_central_bench: a NimBLE CENTRAL bench instrument (connects to a WiCAN,
# tunes the link like esp-idf's throughput_app, runs timed GATT tests).
# Builds only with the NimBLE host in the config; a firmware without BT
# (or with Bluedroid) gets an empty component so composition roots need no
# ifdefs. The pure tunnel client codec always compiles (host tests).
if(CONFIG_BT_NIMBLE_ENABLED)
    set(bcb_srcs "ble_central_bench.c"
                 "ble_central_bench_gap.c"
                 "ble_central_bench_disc.c"
                 "ble_central_bench_modes.c"
                 "ble_central_bench_tunnel.c"
                 "ble_central_bench_settings.c"
                 "ble_central_bench_http.c"
                 "ble_central_bench_cli.c")
else()
    set(bcb_srcs "")
endif()

idf_component_register(
    SRCS ${bcb_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES bt settings_manager log_manager cmdline_manager console
                  http_server_manager esp_http_server espressif__cjson
                  esp_timer nvs_flash
)
