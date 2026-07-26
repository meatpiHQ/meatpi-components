idf_component_register(
    SRCS "translator_realdash.c"
         "translator_realdash_codec.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bridge_manager can_manager
)
