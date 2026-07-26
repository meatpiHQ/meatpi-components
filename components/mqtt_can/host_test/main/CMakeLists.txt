# Pure codec only (mqtt_can_codec.c). can_frame_wire.h/can_core.h come
# from can_manager; cJSON from the registry (idf_component.yml).
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")
set(root "${comp_dir}/..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/mqtt_can_codec.c"
    INCLUDE_DIRS
        "${comp_dir}/include"
        "${root}/can_manager/include"
    REQUIRES unity
)
