# Pure module only: the HTTP-over-BLE CLIENT codec (frame header, request
# head, response head parse, the feed state machine with resync and the
# credit window). No IDF glue, no cJSON.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/ble_central_bench_tunnel.c"
    INCLUDE_DIRS "${comp_dir}" "${comp_dir}/include"
    REQUIRES unity
)
