idf_component_register(
    SRCS "test_lifecycle.c"
    # The inherited main configuration enables the mDNS console commands.
    REQUIRES microlink unity nvs_flash esp_netif esp_event console
)
