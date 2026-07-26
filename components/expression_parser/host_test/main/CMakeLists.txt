# The whole component is pure — the host test compiles it directly.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/expression_parser.c"
    INCLUDE_DIRS "${comp_dir}/include"
    REQUIRES unity
)
