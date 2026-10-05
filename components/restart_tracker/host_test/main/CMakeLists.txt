# Host test compiles ONLY the pure core — no esp_cache/esp_timer needed,
# so it runs on the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_core.c"
         "test_crash_core.c"
         "test_brake_core.c"
         "test_report_core.c"
         "${comp_dir}/restart_tracker_core.c"
         "${comp_dir}/restart_tracker_crash_core.c"
         "${comp_dir}/restart_tracker_brake_core.c"
         "${comp_dir}/restart_tracker_report_core.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
