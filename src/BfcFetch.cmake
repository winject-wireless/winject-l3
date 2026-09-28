include(FetchContent)

set(_bfc_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/bfc-reactor-subms-timeout.patch")

FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    PATCH_COMMAND bash -c "patch -p1 -N --input=${_bfc_patch} || true"
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
