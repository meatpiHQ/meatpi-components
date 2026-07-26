cmake_minimum_required(VERSION 3.16)
# WiCAN components root: ble_manager + bridge_manager + core deps
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
# Standard §7 (rev 2.1): inherit the MAIN firmware's config + test overlay.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ble_manager_test_app)
