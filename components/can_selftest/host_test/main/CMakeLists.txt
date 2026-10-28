# Host test compiles ONLY the pure half: the reply classifier and the verdict.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/can_selftest_parse.c"
    INCLUDE_DIRS "${comp_dir}/include"
    REQUIRES unity
)
