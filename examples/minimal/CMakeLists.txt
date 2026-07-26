cmake_minimum_required(VERSION 3.16)

# Pull every MeatPi component from this repo; the build graph is trimmed
# to main's dependency closure by COMPONENTS below.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meatpi_minimal)
