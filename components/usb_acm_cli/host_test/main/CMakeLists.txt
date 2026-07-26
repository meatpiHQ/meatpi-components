# Pure module only: the ESPNetLink `gps -p -j` parser (usb_acm_gps.c has
# no FreeRTOS/USB deps). This is the mapping GPS values ride into autopid
# (and thence HA / the logger / the dashboard).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/usb_acm_gps.c"
    INCLUDE_DIRS "${comp_dir}/include"
    REQUIRES unity
)
