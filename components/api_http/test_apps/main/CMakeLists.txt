idf_component_register(
    SRCS "test_api_main.c"
    REQUIRES api_http http_server_manager settings_manager dev_status_manager
             restart_tracker log_manager filesystem esp_http_client esp_netif
             nvs_flash
)
