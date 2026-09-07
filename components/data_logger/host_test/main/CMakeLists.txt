# Host test compiles ONLY the pure file-name helpers — no sqlite, no
# RTOS, no VFS.
set(comp_dir "${CMAKE_CURRENT_LIST_DIR}/../..")

idf_component_register(
    SRCS "test_main.c"
         "test_recover.c"
         "${comp_dir}/data_logger_files.c"
         "${comp_dir}/data_logger_recover.c"
    INCLUDE_DIRS "${comp_dir}/include" "${comp_dir}"
    REQUIRES unity
)
