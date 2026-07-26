# Host test compiles ONLY the pure policy layer (backoff, accept decisions,
# config parsing/validation) — no lwIP, so it runs on the linux target.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_policy.c"
         "${comp_dir}/socket_manager_policy.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    # cJSON comes from the espressif/cjson managed dep (idf_component.yml).
    REQUIRES unity
)
