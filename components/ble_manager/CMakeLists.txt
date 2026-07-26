# ble_manager builds only when the BT stack is in the config (the main
# firmware always has CONFIG_BT_ENABLED; test apps of OTHER components may
# not — they discover every component dir but must not drag Bluedroid in).
# The GATT backend follows the sdkconfig BT host choice: Bluedroid
# (ble_manager_gatt.c) or NimBLE (ble_manager_gatt_nimble.c) — switch by
# flipping CONFIG_BT_BLUEDROID_ENABLED/CONFIG_BT_NIMBLE_ENABLED (the
# memory/perf A/B, meatpi 2026-07-05). Same on-air contract either way.
if(CONFIG_BT_ENABLED)
    set(blm_srcs "ble_manager.c"
                 "ble_manager_settings.c"
                 "ble_manager_io.c"
                 "ble_manager_pack.c")
    if(CONFIG_BT_NIMBLE_ENABLED)
        list(APPEND blm_srcs "ble_manager_gatt_nimble.c")
    else()
        list(APPEND blm_srcs "ble_manager_gatt.c")
    endif()
else()
    set(blm_srcs "")
endif()

idf_component_register(
    SRCS ${blm_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Public header exposes only esp_err/FreeRTOS queue types -> all private.
    # (Legacy ble.c stays in this directory as the on-air reference — it is
    # deliberately NOT compiled.)
    PRIV_REQUIRES bt settings_manager log_manager dev_status_manager
)
