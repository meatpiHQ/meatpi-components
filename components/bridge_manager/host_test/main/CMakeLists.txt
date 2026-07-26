# Host test compiles the pure config layer + the translator CONTRACT (via a
# line-splitter test codec defined in the test) — no FreeRTOS runtime use,
# so it runs on the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_config.c"
         "test_translator_contract.c"
         "${comp_dir}/bridge_manager_config.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    # cJSON comes from the espressif/cjson managed dep (idf_component.yml).
    REQUIRES unity
)
