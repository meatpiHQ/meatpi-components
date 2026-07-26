idf_component_register(
    SRCS "test_obd_main.c"
    REQUIRES obd_chip settings_manager log_manager filesystem nvs_flash
             esp_driver_uart esp_driver_gpio
    EMBED_TXTFILES "../../../obd_chip_manager/obd_fw/V2.3.22.txt"
)
