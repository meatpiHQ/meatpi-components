# Build & run on the host:
#   idf.py --preview set-target linux && idf.py build
#   ./build/ble_central_bench_host_test.elf
cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ble_central_bench_host_test)
