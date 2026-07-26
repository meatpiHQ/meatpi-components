# Host test compiles ONLY the pure topic/url logic — no client, no RTOS.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/mqtt_manager_match.c"
         "${comp_dir}/mqtt_manager_item.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
