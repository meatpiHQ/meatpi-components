# Host test: the pure J2534 wire codec + filter match (no I/O), and since
# 2026-10-27 the settings migration (j2534_server_migrate.c, cJSON only:
# v1 -> v2 drops allow_reflash; cJSON via idf_component.yml).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/j2534_proto.c"
         "${comp_dir}/j2534_server_migrate.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
