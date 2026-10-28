# ESP-IDF component: the bandwidth tester app (app_bwtest::main, Bwtest.h) on
# an L3-owned queue upstream. Add <winject-l3>/idf/app_bwtest next to
# idf/winject_l3 in EXTRA_COMPONENT_DIRS.
set(WINJECT_L3_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${WINJECT_L3_ROOT}/src/BfcFetch.cmake)
endif()

idf_component_register(
    SRCS ${WINJECT_L3_ROOT}/src/apps/bwtest/Bwtest.cpp
    INCLUDE_DIRS ${WINJECT_L3_ROOT}/src
                 ${bfc_SOURCE_DIR}/src
    REQUIRES winject_l3
    PRIV_REQUIRES lwip pthread
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
