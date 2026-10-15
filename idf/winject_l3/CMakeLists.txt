# ESP-IDF component: the winject-l3 library (winject_l3::main, WinjectL3.h).
# Add <winject-l3>/idf/winject_l3 to EXTRA_COMPONENT_DIRS of an IDF project.
set(WINJECT_L3_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(WINJECT_L3_MANAGER ${WINJECT_L3_ROOT}/src/manager)

include(${WINJECT_L3_MANAGER}/WinjectL3Sources.cmake)
list(TRANSFORM WINJECT_L3_SOURCES PREPEND ${WINJECT_L3_MANAGER}/)

# BFC (header-only) and ISA-L's portable erasure code, at the versions the
# Linux build uses. Not fetched during IDF's early requirement scan.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(FetchContent)
    FetchContent_Declare(bfc
        GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
        GIT_TAG 9cdd7ba78e088fdc70cef153de2f23b7646f8452
    )
    FetchContent_GetProperties(bfc)
    if(NOT bfc_POPULATED)
        FetchContent_Populate(bfc)
    endif()
    FetchContent_Declare(isa-l
        GIT_REPOSITORY https://github.com/intel/isa-l.git
        GIT_TAG v2.31.1
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(isa-l)
    if(NOT isa-l_POPULATED)
        FetchContent_Populate(isa-l)
    endif()

    include(${WINJECT_L3_ROOT}/cmake/WinjectVersion.cmake)
    configure_file(${WINJECT_L3_MANAGER}/WinjectBuildVersion.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/generated/WinjectBuildVersion.h @ONLY)
endif()

idf_component_register(
    SRCS ${WINJECT_L3_SOURCES}
         ${isa-l_SOURCE_DIR}/erasure_code/ec_base.c
         port/pipe.c
    INCLUDE_DIRS ${WINJECT_L3_MANAGER}
    PRIV_INCLUDE_DIRS compat
                      ${WINJECT_L3_ROOT}/src
                      ${bfc_SOURCE_DIR}/src
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
