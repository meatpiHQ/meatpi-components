idf_component_register(
    SRCS "can_manager.c"
         "can_manager_probe.c"
         "can_manager_settings.c"
         "can_manager_http.c"
         "can_manager_cli.c"
         "can_isotp.c"
         "can_core.c"
         "can_core_clients.c"
         "can_core_driver.c"
         "can_core_link.c"
         "can_core_filter.c"
         "can_core_recovery.c"
         "can_autobaud_core.c"
         "can_timing_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Service: owns the TWAI bus. can_core* is the adopted shared-bus
    # engine (ex elm327_can component, merged 2026-07-19 — PROVENANCE.md);
    # cmdline/http only by the surface files (§6b + §9.1).
    REQUIRES esp_driver_twai
    # esp_hal_twai + soc: the receive-error storm throttle masks the
    # controller's interrupts from the ISR with the HAL's register access
    # (twai_ll_set_enabled_intrs on TWAI), can_core_driver.c
    PRIV_REQUIRES settings_manager log_manager esp_driver_gpio
                  cmdline_manager console http_server_manager
                  esp_http_server esp_timer esp_hal_twai soc
)

# The IDF node driver drops frames of an overrun controller FIFO without a
# counter. can_core_driver.c counts them by wrapping the HAL read the
# driver's interrupt calls (__wrap_twai_hal_read_rx_fifo, stats rx_overrun).
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=twai_hal_read_rx_fifo")
