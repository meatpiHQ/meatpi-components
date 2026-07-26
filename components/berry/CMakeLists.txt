# Vendored Berry VM (github.com/berry-lang/berry, MIT). See PROVENANCE.md.
# The coc-generated headers in generate/ are pre-built and vendored.
file(GLOB BERRY_SRCS "src/*.c")

idf_component_register(
    SRCS ${BERRY_SRCS}
         "default/be_port.c"   # file I/O (bytecode/parser) + be_writebuffer
                               # patched to route through berry_port_write
         "default/be_modtab.c"
         "berry_esp_port.c"    # PSRAM heap + the capture-able writer
    INCLUDE_DIRS "src" "default"
    PRIV_INCLUDE_DIRS "generate"
    # PSRAM heap via heap_caps; the console hook uses the log.
    PRIV_REQUIRES esp_mm
)

# Berry casts + its own conventions trip a few -Werror pedantic checks;
# it's third-party, keep it building without patching the vendor tree.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=all -Wno-error=extra -Wno-error=unused-function
    -Wno-error=sign-compare -Wno-error=char-subscripts -Wno-format
)
