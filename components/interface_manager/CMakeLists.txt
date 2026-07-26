idf_component_register(
    SRCS "interface_manager.c"
         "interface_manager_settings.c"
         "interface_manager_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Policy: THE place wireless interface on/off rules live (BLE<->STA
    # handover, AP<->BLE exclusivity; future: USB kills radios). Sits
    # above wifi_manager/ble_manager and actuates their runtime APIs.
    PRIV_REQUIRES wifi_manager ble_manager dev_status_manager
                  settings_manager log_manager
)
