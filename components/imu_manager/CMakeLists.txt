idf_component_register(
    SRCS "imu_manager.c"
         "imu_manager_events.c"
         "imu_manager_policy.c"
         "imu_manager_settings.c"
         "imu_manager_migrate.c"
         "imu_manager_http.c"
         "imu_manager_cli.c"
         "icm42670/icm42670.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "icm42670"
    # Feature: owns the ICM-42670 IMU on the shared bus (i2c_bus) —
    # SMD/WoM detectors POLLED via INT_STATUS2 (no ISR, no GPIO dep) ->
    # activity state + subscriber events. /api/imu and the `imu` command
    # are optional (main wires them). icm42670/ = vendored espressif
    # driver (PROVENANCE.md).
    PRIV_REQUIRES event_manager i2c_bus esp_driver_i2c esp_timer http_server_manager
                  dev_status_manager settings_manager log_manager
                  cmdline_manager
)
