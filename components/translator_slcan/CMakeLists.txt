idf_component_register(
    SRCS "translator_slcan.c"
         "translator_slcan_codec.c"
    INCLUDE_DIRS "include"
    # bridge_manager: the translator contract + registration.
    # can_manager/can_core: the shared can_frame_wire.h + can_core_frame_t.
    PRIV_REQUIRES bridge_manager can_manager
)
