cmake_minimum_required(VERSION 3.16)
project(winject-manager VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/WinjectVersion.cmake)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/WinjectBuildVersion.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/WinjectBuildVersion.h
    @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../BfcFetch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/fec/IsalEc.cmake)
winject_add_isal_ec()

include(${CMAKE_CURRENT_SOURCE_DIR}/WinjectL3Sources.cmake)

# Library: everything but main(); entry point winject_l3::main (WinjectL3.h).
add_library(winject-l3 STATIC ${WINJECT_L3_SOURCES})

target_include_directories(winject-l3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${bfc_SOURCE_DIR}/src
)

# No RTTI: the ESP-IDF build has it off too.
target_compile_options(winject-l3 PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -Wno-missing-field-initializers
    -fno-rtti
)

target_link_libraries(winject-l3 PUBLIC pthread isal_ec)

# Binary: a thin main() that calls winject_l3::main.
add_executable(winject-manager Main.cpp)
target_compile_options(winject-manager PRIVATE -Wall -Wextra -fno-rtti)
target_link_libraries(winject-manager PRIVATE winject-l3)
