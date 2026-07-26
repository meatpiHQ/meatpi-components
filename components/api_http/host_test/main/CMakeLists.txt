# Host test compiles ONLY the pure utility layer (redaction, route parsing,
# level mapping) — no esp_http_server needed, so it runs on the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_util.c"
         "${comp_dir}/api_http_util.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    # cJSON comes from the espressif/cjson managed dep (idf_component.yml);
    # IDF v6 dropped the bundled `json` component.
    REQUIRES unity
)
