idf_component_register(
    SRCS "bridge_endpoints.c"
         "bridge_endpoints_usb.c"
         "bridge_endpoints_can.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # The adapter layer: knows BOTH the providers and bridge_manager (the
    # one component allowed to — providers themselves stay bridge-agnostic,
    # ARCHITECTURE §10). Widest fan-in in the tree by design.
    PRIV_REQUIRES bridge_manager obd_chip ble_manager can_manager
                  cmdline_manager socket_manager
                  websocket_manager log_manager esp_driver_uart
)
