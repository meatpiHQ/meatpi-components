idf_component_register(
    SRCS "test_bridge_main.c"
    REQUIRES bridge_manager socket_manager settings_manager log_manager lwip
             esp_netif esp_timer nvs_flash
)
