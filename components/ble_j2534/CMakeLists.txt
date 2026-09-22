# ble_j2534: J2534 PassThru over a ble_manager stream channel. Builds only
# with the BT stack in the config, like ble_manager.
if(CONFIG_BT_ENABLED)
    set(blj_srcs "ble_j2534.c")
else()
    set(blj_srcs "")
endif()

idf_component_register(
    SRCS ${blj_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ble_manager j2534_server log_manager
)
