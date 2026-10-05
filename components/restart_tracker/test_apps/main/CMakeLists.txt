idf_component_register(
    SRCS "test_rt_main.c"
         "test_rt_crash_cases.c"
    # the crash cases: a timer interrupt, the flash cache switch, the
    # cache write-back of the run's progress; nvs_flash: the stored crash
    # report goes to the DUT's own NVS partition
    REQUIRES restart_tracker esp_driver_gptimer spi_flash esp_mm esp_timer
             nvs_flash
)

# Probes around the crash note, in THIS app's link only (the component
# carries no test hook): the time stage B takes, and a fault or a hang
# inside it (test_rt_crash_cases.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=xPortInterruptedFromISRContext"
                      "-Wl,--wrap=xTaskGetCurrentTaskHandleForCore"
                      "-Wl,--wrap=esp_restart_noos")
