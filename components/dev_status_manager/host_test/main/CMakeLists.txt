# Host test compiles ONLY the pure helpers (bit names + uptime formatting,
# the fault count and its cap).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_fmt.c"
         "test_fault_count.c"
         "${comp_dir}/dev_status_manager_fmt.c"
         "${comp_dir}/dev_status_fault_count.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
