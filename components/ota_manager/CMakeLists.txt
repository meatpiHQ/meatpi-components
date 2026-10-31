idf_component_register(
    SRCS "ota_manager.c"
         "ota_session.c"
         "ota_pipe.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # flash_jobs: every esp_ota call runs on an internal stack whoever the
    # caller is (2026-10-31)
    PRIV_REQUIRES app_update log_manager mbedtls flash_jobs
)
