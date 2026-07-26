# web_ui_v2 — the v2 single-page app, embedded (gzipped) and served by the
# http_server_manager catch-all. Compiled only when CONFIG_WICAN_WEBUI_V2 is
# selected (the Kconfig choice); otherwise this is an empty component so the
# build + main's web_ui_v2_register() stub still link.
set(srcs "web_ui_v2.c")
set(embeds "")

if(CONFIG_WICAN_WEBUI_V2)
    # Pre-gzip index.html into the build dir, then embed the .gz. Portable:
    # uses the IDF Python (no external gzip binary needed on Windows).
    idf_build_get_property(python PYTHON)
    set(src_html "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
    set(gz_html  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
    add_custom_command(
        OUTPUT "${gz_html}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
                "${src_html}" "${gz_html}"
        DEPENDS "${src_html}" "${CMAKE_CURRENT_LIST_DIR}/gzip_asset.py"
        COMMENT "Gzipping web_ui_v2 index.html for embedding"
        VERBATIM)
    add_custom_target(web_ui_v2_gz DEPENDS "${gz_html}")
    set(embeds "${gz_html}")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES http_server_manager
    EMBED_FILES ${embeds}
)

if(CONFIG_WICAN_WEBUI_V2)
    add_dependencies(${COMPONENT_LIB} web_ui_v2_gz)
endif()
