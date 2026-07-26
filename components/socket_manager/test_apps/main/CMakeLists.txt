idf_component_register(
    SRCS "test_socket_main.c"
    REQUIRES socket_manager settings_manager log_manager lwip esp_netif
             nvs_flash
)
