# Pure modules only: the CAN filter/mask match + id/byte parse + hex format
# helpers (can_core_filter.c) plus the bus-off recovery restart policy
# (can_core_recovery.c), the listen-before-talk bitrate / mode policy
# (can_autobaud_core.c) and the bit timing (can_timing_core.c) — no
# FreeRTOS/TWAI deps in any. This is the
# logic can_manager and the ELM327 path dispatch RX frames through, and
# the backoff decisions the RX task applies after a bus-off.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_autobaud.c"
         "test_timing.c"
         "${comp_dir}/can_core_filter.c"
         "${comp_dir}/can_core_recovery.c"
         "${comp_dir}/can_autobaud_core.c"
         "${comp_dir}/can_timing_core.c"
    INCLUDE_DIRS "${comp_dir}/include"
    REQUIRES unity
)
