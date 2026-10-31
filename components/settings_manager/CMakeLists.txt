idf_component_register(
    SRCS "settings_manager.c"
         "settings_manager_boot.c"
         "settings_manager_backup.c"
         "settings_manager_schema.c"
         "settings_manager_codec.c"
         "settings_manager_fields.c"
         "settings_manager_storage.c"
         "settings_manager_lfs.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # flash_jobs: the settings files are read and written on an internal
    # stack whoever the caller is (2026-10-31)
    PRIV_REQUIRES littlefs flash_jobs
)
