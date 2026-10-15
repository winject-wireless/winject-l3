# Minimal ESP32-P4 project that builds the winject-l3 component and runs
# winject_l3::main on a pthread. Build: idf.py set-target esp32p4 build
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../winject_l3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(winject-l3-idf-example)
