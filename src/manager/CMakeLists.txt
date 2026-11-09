cmake_minimum_required(VERSION 3.16)
project(winject-manager VERSION 1.0.0 LANGUAGES CXX)

set(WINJECT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/..)

include(${WINJECT_SRC}/../cmake/WinjectVersion.cmake)
configure_file(
    ${WINJECT_SRC}/core/WinjectBuildVersion.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/WinjectBuildVersion.h
    @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${WINJECT_SRC}/BfcFetch.cmake)

include(${WINJECT_SRC}/core/fec/IsalEc.cmake)
winject_add_isal_ec()

include(${WINJECT_SRC}/core/CoreSources.cmake)
include(${WINJECT_SRC}/l3/L3Sources.cmake)
list(TRANSFORM WINJECT_CORE_SOURCES PREPEND ${WINJECT_SRC}/core/)
list(TRANSFORM WINJECT_L3_SOURCES PREPEND ${WINJECT_SRC}/l3/)

# No RTTI: the ESP-IDF build has it off too.
set(WINJECT_WARNINGS
    -Wall
    -Wextra
    -Werror=return-type
    -Wno-missing-field-initializers
    -fno-rtti
)

# Core: data path + radio m-plane client. Knows nothing of Config files or
# the l3 console.
add_library(winject-l3-core STATIC ${WINJECT_CORE_SOURCES})
target_include_directories(winject-l3-core PUBLIC
    ${WINJECT_SRC}/core
    ${CMAKE_CURRENT_BINARY_DIR}
    ${BFC_INCLUDE_DIRS}
)
target_compile_options(winject-l3-core PRIVATE ${WINJECT_WARNINGS})
target_link_libraries(winject-l3-core PUBLIC pthread isal_ec)

# L3: the l3 applet on top of the core, its public API (include/winject_l3/*.h:
# winject::L3, managers, handles) and the text console.
add_library(winject-l3 STATIC ${WINJECT_L3_SOURCES})
target_include_directories(winject-l3 PUBLIC
    ${WINJECT_SRC}/../include
    ${WINJECT_SRC}/l3
    ${WINJECT_SRC}
)
target_compile_options(winject-l3 PRIVATE ${WINJECT_WARNINGS})
target_link_libraries(winject-l3 PUBLIC winject-l3-core)

# Binary: main() reads the config file, runs l3 and its UDP console.
add_executable(winject-manager Main.cpp)
target_compile_options(winject-manager PRIVATE -Wall -Wextra -fno-rtti)
target_link_libraries(winject-manager PRIVATE winject-l3)
