idf_component_register(
    SRCS "main.c"
    REQUIRES nvs_flash log_manager settings_manager cmdline_manager
             can_manager obd_gate ext_manager
)
