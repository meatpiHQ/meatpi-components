# Pure module only: frame codec, request/response heads, the tunnel state
# machine against a fake HTTP side and a capturing emit. No IDF glue.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/ble_http_core.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
