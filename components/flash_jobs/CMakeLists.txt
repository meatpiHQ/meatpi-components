idf_component_register(
    SRCS "flash_jobs.c"
         "flash_jobs_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # esp_hw_support: esp_ptr_internal (where the caller's stack lives);
    # esp_timer: the clock of the refusal log rate limit; log_manager: the
    # log descriptor
    PRIV_REQUIRES esp_hw_support esp_timer log_manager
)
