# Pure modules only: the ESPNetLink `gps -p -j` parser (usb_acm_gps.c has
# no FreeRTOS/USB deps) and, since 2026-10-07, the NMEA 0183 reader for a
# plain USB GNSS receiver (usb_acm_nmea.c). Both are the mapping GPS values
# ride into autopid (and thence HA / the logger / the dashboard).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/usb_acm_gps.c"
         "${comp_dir}/usb_acm_nmea.c"
    INCLUDE_DIRS "${comp_dir}/include"
    REQUIRES unity
)
