# Host test: the pure parse/framing, fw-version and sleep-config modules, and
# (test_request.c) the production request engine, claim model and fan-out
# against a fake chip that stands in for the UART: no UART/GPIO deps.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_parse.c"
         "test_stslcs.c"
         "test_request.c"
         "test_kline.c"
         "${comp_dir}/obd_chip_cmd.c"
         "${comp_dir}/obd_chip_claim.c"
         "${comp_dir}/obd_chip_fanout.c"
         "${comp_dir}/obd_chip_parse.c"
         "${comp_dir}/obd_chip_class.c"
         "${comp_dir}/obd_chip_kline.c"
         "${comp_dir}/obd_chip_fw_version.c"
         "${comp_dir}/obd_chip_stslcs.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}" "${comp_dir}/../obd_gate/include"
    REQUIRES unity esp_timer
)

# OBD_CHIP_HOST_TEST: the driver's test-only hook (obd_cmd_test_new_boot, the
# per-boot state of the protocol save as a restart leaves it; standard 3).
target_compile_definitions(${COMPONENT_LIB} PRIVATE OBD_CHIP_HOST_TEST=1)
