# Pure modules only: the dongle document parsers, the VID/PID identity,
# URL build and the pairing state machine (espnetlink_link_core.c has no
# IDF deps), the settings migration (cJSON only) + the shared GPS parser
# fed the dongle's HTTP `/api/gps` shape (latitude/longitude keys).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_sm.c"
         "${comp_dir}/espnetlink_link_core.c"
         "${comp_dir}/espnetlink_link_migrate.c"
         "${comp_dir}/../usb_acm_cli/usb_acm_gps.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/../usb_acm_cli/include"
    REQUIRES unity
)
