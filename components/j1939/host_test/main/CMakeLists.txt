# Pure modules only: the identifier / frame kinds / bus evidence / VIN /
# request and acknowledgment (j1939_core.c), transport protocol reassembly
# and the destination role (j1939_tp_core.c), DM1 / DM2 decode
# (j1939_dm_core.c), the value table (j1939_spn_core.c), the message store
# (j1939_cache_core.c) and the address claim (j1939_claim_core.c). No
# FreeRTOS, no CAN driver, no clock.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_tp.c"
         "test_values.c"
         "test_cache.c"
         "test_active.c"
         "${comp_dir}/j1939_core.c"
         "${comp_dir}/j1939_tp_core.c"
         "${comp_dir}/j1939_dm_core.c"
         "${comp_dir}/j1939_spn_core.c"
         "${comp_dir}/j1939_cache_core.c"
         "${comp_dir}/j1939_claim_core.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
