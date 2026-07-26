cmake_minimum_required(VERSION 3.16)
# WiCAN components root: bridge_manager + socket_manager + core deps
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
# Standard §7 (rev 2.1): inherit the MAIN firmware's config, then the small
# test-only overlay in this directory.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bridge_manager_test_app)
