# Compiles ONLY the pure core (ring + batcher + rotation planner) — no
# lwip/FreeRTOS/VFS needed on the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/log_sinks_core.c"
    INCLUDE_DIRS "${comp_dir}"
    REQUIRES unity
)
