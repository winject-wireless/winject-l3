# Minimal ESP32-P4 project that builds the winject-l3 component and runs
# winject::L3::run on a pthread. Build: idf.py set-target esp32p4 build
cmake_minimum_required(VERSION 3.16)
# bfc and winject-fec at this repo's pins; the winject_fec component comes from
# the winject-fec checkout.
include(${CMAKE_CURRENT_LIST_DIR}/../../src/BfcFetch.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/WinjectFecFetch.cmake)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../winject_l3
                         ${WINJECT_FEC_SOURCE_DIR}/idf/winject_fec)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(winject-l3-idf-example)
