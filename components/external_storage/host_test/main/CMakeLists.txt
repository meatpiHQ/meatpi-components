# Host test compiles ONLY the pure parts: the detect debouncer, the probe
# of what a card holds and the rule of the close guard. No GPIO/SDMMC.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "${comp_dir}/es_detect.c"
         "${comp_dir}/es_probe.c"
         "${comp_dir}/es_close_core.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
