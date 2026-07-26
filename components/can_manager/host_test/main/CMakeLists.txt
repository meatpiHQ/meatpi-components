# Pure modules only: the CAN filter/mask match + id/byte parse + hex format
# helpers (can_core_filter.c) plus the bus-off recovery restart policy
# (can_core_recovery.c) — no FreeRTOS/TWAI deps in either. This is the
# logic can_manager and the ELM327 path dispatch RX frames through, and
# the backoff decisions the RX task applies after a bus-off.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/can_core_filter.c"
         "${comp_dir}/can_core_recovery.c"
    INCLUDE_DIRS "${comp_dir}/include"
    REQUIRES unity
)
