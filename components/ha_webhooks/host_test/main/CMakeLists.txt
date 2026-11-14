# Host test compiles ONLY the pure rules: the registration object, the
# revision and the push reply (ha_webhooks_reg.c: strings and cJSON, no IDF
# glue).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/ha_webhooks_reg.c"
    INCLUDE_DIRS "${comp_dir}"
    REQUIRES unity
)
