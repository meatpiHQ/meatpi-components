idf_component_register(
    SRCS "filesystem.c"
         "filesystem_read.c"
         "filesystem_settings.c"
         "filesystem_stream.c"
         "filesystem_path.c"
         "filesystem_mount.c"
         "filesystem_cli.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # flash_jobs: the internal-stack worker every /data operation of a
    # PSRAM-stack caller runs on (2026-10-31)
    PRIV_REQUIRES littlefs vfs fatfs log_manager cmdline_manager
                  settings_manager flash_jobs
)
