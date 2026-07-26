cmake_minimum_required(VERSION 3.16)
# WiCAN components root + the vendored sqlite port (still in legacy/ during
# the perf-characterization phase; it moves into data_logger when built).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../legacy/nopnop2002__sqlite3")
# Standard §7 (rev 2.1): inherit the MAIN firmware's config + a small overlay.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(data_logger_bench)
