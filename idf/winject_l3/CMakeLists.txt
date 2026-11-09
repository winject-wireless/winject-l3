# ESP-IDF component: the winject-l3 core, the l3 applet and its public API
# (include/winject_l3/*.h: winject::L3, managers, handles, console).
# Add <winject-l3>/idf/winject_l3 to EXTRA_COMPONENT_DIRS of an IDF project.
set(WINJECT_L3_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(WINJECT_CORE_DIR ${WINJECT_L3_ROOT}/src/core)
set(WINJECT_L3_DIR ${WINJECT_L3_ROOT}/src/l3)

include(${WINJECT_CORE_DIR}/CoreSources.cmake)
include(${WINJECT_L3_DIR}/L3Sources.cmake)
list(TRANSFORM WINJECT_CORE_SOURCES PREPEND ${WINJECT_CORE_DIR}/)
list(TRANSFORM WINJECT_L3_SOURCES PREPEND ${WINJECT_L3_DIR}/)

# BFC (header-only) and ISA-L's portable erasure code, pinned once in
# src/BfcFetch.cmake and fec/IsalEc.cmake. Not fetched during IDF's early
# requirement scan.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${WINJECT_L3_ROOT}/src/BfcFetch.cmake)
    include(${WINJECT_CORE_DIR}/fec/IsalEc.cmake)
    winject_fetch_isal()
    FetchContent_GetProperties(isa-l)

    include(${WINJECT_L3_ROOT}/cmake/WinjectVersion.cmake)
    configure_file(${WINJECT_CORE_DIR}/WinjectBuildVersion.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/generated/WinjectBuildVersion.h @ONLY)
endif()

idf_component_register(
    SRCS ${WINJECT_CORE_SOURCES}
         ${WINJECT_L3_SOURCES}
         ${isa-l_SOURCE_DIR}/erasure_code/ec_base.c
         port/pipe.c
    # compat: <poll.h> for BFC's reactor, which the public Console.h uses.
    # bfc is public: include/winject_l3/Console.h uses its reactor.
    INCLUDE_DIRS ${WINJECT_L3_ROOT}/include
                 ${BFC_INCLUDE_DIRS}
                 ${WINJECT_CORE_DIR}
                 ${WINJECT_L3_DIR}
                 compat
    PRIV_INCLUDE_DIRS ${WINJECT_L3_ROOT}/src
                      ${isa-l_SOURCE_DIR}/include
                      ${isa-l_SOURCE_DIR}/erasure_code
                      ${CMAKE_CURRENT_BINARY_DIR}/generated
    PRIV_REQUIRES lwip pthread vfs
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-missing-field-initializers
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    # BFC throws on fatal errors; with IDF's C++ exceptions off a throw aborts.
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
set_source_files_properties(${isa-l_SOURCE_DIR}/erasure_code/ec_base.c
    PROPERTIES COMPILE_DEFINITIONS NDEBUG)
