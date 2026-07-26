# Host test: the pure J2534 wire codec + filter match (no I/O).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/j2534_proto.c"
    INCLUDE_DIRS "${comp_dir}/include"
    REQUIRES unity
)
