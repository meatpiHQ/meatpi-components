# ext_manager — boot hooks for optional add-on component packs.
# Stock build: no-ops. A pack overlays this component (same name, later
# component dir) with an implementation that registers the pack's
# features; see include/ext_manager.h for the contract with main.
idf_component_register(
    SRCS "ext_manager.c"
    INCLUDE_DIRS "include"
)
