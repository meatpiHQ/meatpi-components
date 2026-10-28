idf_component_register(
    SRCS "ip4_routes.c"
         "ip4_routes_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # lwip: the netif type and the hook's address types; log_manager: the
    # log descriptor
    PRIV_REQUIRES lwip log_manager
)

# IDF's lwIP port defines LWIP_HOOK_IP4_ROUTE_SRC as its own
# ip4_route_src_hook (source-address routing; a strong symbol with no
# Kconfig choice). The destination route table gets in front of it by
# wrapping the symbol at link time (the restart_tracker / can_manager
# pattern): lwIP's calls land in __wrap_ip4_route_src_hook, which falls
# through to __real_ip4_route_src_hook when the table has no answer.
# The -u beside the wrap (IDF's own idiom, esp_system's "-u start_app")
# makes the linker take the wrapper out of this static library at its
# first scan. Without it a link that nothing else ties to ip4_routes (an
# on-target test app, which builds every component in the tree) applies
# the wrap, scans libip4_routes.a before lwIP needs the wrapper and ends
# in "undefined reference to __wrap_ip4_route_src_hook" (every test app
# since the wrap landed; found 2026-10-29). main links the wrapper either
# way, through its own ip4_routes calls.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=ip4_route_src_hook"
                      "-u __wrap_ip4_route_src_hook")
