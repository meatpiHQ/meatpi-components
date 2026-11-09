set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c" "test_routes.c"
         "${comp_dir}/src/ml_disco_pong.c" "${comp_dir}/src/ml_routes.c"
    # ip4_routes: its header only (the entry type ml_routes.c fills)
    INCLUDE_DIRS "stubs" "${comp_dir}/src" "${comp_dir}/../ip4_routes/include"
    REQUIRES unity
)
