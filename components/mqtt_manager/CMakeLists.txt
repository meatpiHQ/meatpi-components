idf_component_register(
    SRCS "mqtt_manager.c"
         "mqtt_manager_settings.c"
         "mqtt_manager_match.c"
         "mqtt_manager_item.c"
         "mqtt_manager_events.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # Feature: owns THE one MQTT client (esp-mqtt is a MANAGED component —
    # un-bundled in IDF v6, see idf_component.yml); consumers publish
    # through it (direct or the never-blocking async ring) and register
    # topic-filter handlers into it.
    PRIV_REQUIRES espressif__mqtt mbedtls filesystem esp_ringbuf
                  cert_manager dev_status_manager settings_manager
                  log_manager event_manager
)
