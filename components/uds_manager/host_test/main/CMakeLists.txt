# Host test: the pure UDS proto + AT response parser + DTC codec (no I/O).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/uds_proto.c"
         "${comp_dir}/uds_transport_at.c"
         "${comp_dir}/uds_dtc_codec.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
