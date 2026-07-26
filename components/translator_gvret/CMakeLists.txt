idf_component_register(
    SRCS "translator_gvret.c"
         "translator_gvret_codec.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bridge_manager can_manager
)
