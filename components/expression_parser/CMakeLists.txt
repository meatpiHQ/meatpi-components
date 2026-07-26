idf_component_register(
    SRCS "expression_parser.c"
    INCLUDE_DIRS "include"
    # Pure utility (Automate expression grammar): no dependencies beyond
    # esp_common's esp_err.h — host-testable unchanged on the linux target.
)
