# Host test compiles ONLY the pure policy ladder and the pure watch on the
# OBD chip through the naps: no GPIO, no RTOS.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_chip_watch.c"
         "${comp_dir}/sleep_manager_policy.c"
         "${comp_dir}/sleep_manager_chip.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
