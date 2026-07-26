idf_component_register(
    SRCS "mqtt_can.c"
         "mqtt_can_codec.c"
    INCLUDE_DIRS "include"
    # bridge_manager: endpoint/translator contracts + chunk type.
    # bridge_endpoints: bep_can_set_filter (the 1:1 filter mapping).
    # can_manager: can_frame_wire.h + can_core_frame_t.
    # mqtt_manager: publish_async + topic handler + prefix.
    PRIV_REQUIRES bridge_manager bridge_endpoints can_manager
                  mqtt_manager settings_manager cmdline_manager
    # the codec needs cJSON directly for the tx parse
    REQUIRES espressif__cjson
)
