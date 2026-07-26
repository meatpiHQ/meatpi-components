idf_component_register(
    SRCS "bench_main.c"
    # sqlite3 = the vendored nopnop2002 port (EXTRA_COMPONENT_DIRS);
    # littlefs = managed joltwallet (SDMMC support from the root config)
    PRIV_REQUIRES nopnop2002__sqlite3 fatfs sdmmc esp_driver_sdmmc esp_timer vfs
)
