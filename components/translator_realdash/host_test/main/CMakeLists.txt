set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
set(root "${comp_dir}/..")
idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/translator_realdash_codec.c"
    INCLUDE_DIRS
        "${root}/bridge_manager/include"
        "${root}/can_manager/include"
    REQUIRES unity
)
