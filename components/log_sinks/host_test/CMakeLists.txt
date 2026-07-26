cmake_minimum_required(VERSION 3.16)

# Host-side unit tests (linux target). Restrict the build to main so the
# linux preview target doesn't try to resolve hardware components.
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(log_sinks_host_test)
