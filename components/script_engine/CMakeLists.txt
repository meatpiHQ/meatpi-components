idf_component_register(
    SRCS "script_engine.c"
         "script_engine_name.c"
         "script_engine_obd.c"
         "script_engine_settings.c"
         "script_engine_bind.c"
         "script_engine_bind_can.c"
         "script_engine_gpio.c"
         "script_engine_doc.c"
         "script_engine_examples.c"
         "script_engine_http.c"
         "script_engine_cli.c"
         "script_engine_events.c"
         "script_engine_tp_core.c"
         "script_engine_tp.c"
         "script_engine_flash_core.c"
         "script_engine_flash.c"
         "script_engine_flash_bind.c"
         "script_engine_flash_bind_data.c"
         "script_engine_flash_bind_run.c"
         "script_engine_flash_file.c"
         "script_engine_doc_flash.c"
         "script_engine_http_flash.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: the Berry VM (berry) with device bindings (uds/can/event/dtc).
    PRIV_REQUIRES berry uds_manager can_manager event_manager filesystem obd_gate flash_jobs
                  autopid
                  settings_manager log_manager esp_timer esp_mm
                  http_server_manager esp_http_server espressif__cjson
                  cmdline_manager console
                  esp_driver_gpio esp_driver_rmt esp_hw_support esp_system
                  # ECU flashing (2026-11-27): the lock's NVS record, SHA-256,
                  # SD files, the sleep check, µs pacing
                  nvs_flash mbedtls external_storage sleep_manager esp_rom
                  # the commit waits for no firmware update (2026-11-27)
                  ota_manager obd_chip
)
