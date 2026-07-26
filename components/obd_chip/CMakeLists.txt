idf_component_register(
    SRCS "obd_chip.c"
         "obd_chip_events.c"
         "obd_chip_uart.c"
         "obd_chip_cmd.c"
         "obd_chip_parse.c"
         "obd_chip_stslcs.c"
         "obd_chip_settings.c"
         "obd_chip_fw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # public header exposes FreeRTOS queue types only (implicit common req)
    PRIV_REQUIRES event_manager esp_driver_uart esp_driver_gpio settings_manager obd_gate
                  log_manager filesystem
    # The packaged MIC3624 image (legacy parity, Ali 2026-07-26: a chip
    # not at this version is auto-flashed to it after bring-up). Bumping
    # the packaged version = update this path AND OBD_FW_BUILTIN_VERSION
    # in obd_chip_fw.c. Byte-identical to legacy main/obd_fw/V2.3.22.txt.
    EMBED_TXTFILES "../obd_chip_manager/obd_fw/V2.3.22.txt"
)
