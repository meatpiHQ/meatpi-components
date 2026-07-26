idf_component_register(
    SRCS "bench_main.c"
    REQUIRES bridge_manager socket_manager wifi_manager obd_chip
             settings_manager log_manager dev_status_manager esp_netif
             nvs_flash
)

# Bench AP credentials are injected from the environment so no secret is
# hardcoded in the tree (see components/TESTBENCH.md for the values).
if(DEFINED ENV{WICAN_BENCH_SSID})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BENCH_STA_SSID="$ENV{WICAN_BENCH_SSID}")
endif()
if(DEFINED ENV{WICAN_BENCH_PSK})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BENCH_STA_PASS="$ENV{WICAN_BENCH_PSK}")
else()
    message(WARNING "WICAN_BENCH_PSK not set — bench app will be built "
                    "with a placeholder STA password and will NOT join "
                    "the bench AP")
endif()
