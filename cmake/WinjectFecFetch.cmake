# winject-fec (the FEC streams and codecs), pinned once per build. Include after
# src/BfcFetch.cmake: winject-fec uses the same bfc (BFC_PIN, BFC_INCLUDE_DIRS).
#
# A top-level project that also builds winject-fec for another component
# (vstreamer, a firmware) sets WINJECT_FEC_PIN first; it must match this repo's.
# Sets WINJECT_FEC_SOURCE_DIR. Host builds then add_subdirectory() it; ESP-IDF
# projects add ${WINJECT_FEC_SOURCE_DIR}/idf/winject_fec to EXTRA_COMPONENT_DIRS.
include_guard(GLOBAL)
include(FetchContent)

set(WINJECT_L3_WINJECT_FEC_PIN d30e83171a436f2bec6e9d5ed7ddb38a45dcfccb)

if(NOT DEFINED WINJECT_FEC_PIN)
    set(WINJECT_FEC_PIN ${WINJECT_L3_WINJECT_FEC_PIN})
endif()
if(NOT "${WINJECT_FEC_PIN}" STREQUAL "${WINJECT_L3_WINJECT_FEC_PIN}")
    message(FATAL_ERROR "winject-l3 pins winject-fec ${WINJECT_L3_WINJECT_FEC_PIN}, but the build uses ${WINJECT_FEC_PIN}. Bump this repo's pin to match.")
endif()

FetchContent_Declare(winject_fec
    GIT_REPOSITORY https://github.com/winject-wireless/winject-fec.git
    GIT_TAG ${WINJECT_FEC_PIN}
)
FetchContent_GetProperties(winject_fec)
if(NOT winject_fec_POPULATED)
    FetchContent_Populate(winject_fec)
endif()
execute_process(COMMAND git -C ${winject_fec_SOURCE_DIR} rev-parse HEAD
    OUTPUT_VARIABLE WINJECT_FEC_CHECKED_OUT OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT WINJECT_FEC_CHECKED_OUT STREQUAL WINJECT_FEC_PIN)
    message(FATAL_ERROR "winject-fec at ${winject_fec_SOURCE_DIR} is ${WINJECT_FEC_CHECKED_OUT}, expected ${WINJECT_FEC_PIN}. Delete it and reconfigure.")
endif()
set(WINJECT_FEC_SOURCE_DIR ${winject_fec_SOURCE_DIR})

# Host builds: the static library (winject::fec) without its tests.
function(winject_add_fec)
    if(TARGET winject_fec)
        return()
    endif()
    set(WINJECT_FEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${WINJECT_FEC_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_winject_fec EXCLUDE_FROM_ALL)
endfunction()
