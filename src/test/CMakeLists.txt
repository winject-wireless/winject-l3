cmake_minimum_required(VERSION 3.16)
project(winject-tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../BfcFetch.cmake)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

include(${CMAKE_CURRENT_SOURCE_DIR}/../manager/fec/IsalEc.cmake)
winject_add_isal_ec()

add_executable(winject-tests
    MpduSlotTest.cpp
    MpduTest.cpp
    ConfigTest.cpp
    RsBlockErasureTest.cpp
    NetUtilTest.cpp
    LCSequenceTest.cpp
    ConsoleServiceTest.cpp
    MetricsRegistryTest.cpp
    SharedSizedBufferTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/Config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/frames/Frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/frames/Mpdu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/fec/RsBlockErasure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/endpoint/UpstreamStats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/console/ConsoleParse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/utils/Log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/utils/NetUtil.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager/utils/MetricsRegistry.cpp
)

target_include_directories(winject-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../manager
    ${bfc_SOURCE_DIR}/src
)
target_compile_options(winject-tests PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
)
target_link_libraries(winject-tests PRIVATE GTest::gtest_main pthread isal_ec)

enable_testing()
include(GoogleTest)
gtest_discover_tests(winject-tests)
