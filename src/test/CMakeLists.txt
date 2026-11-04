cmake_minimum_required(VERSION 3.16)
project(winject-tests CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/WinjectVersion.cmake)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/WinjectBuildVersion.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/WinjectBuildVersion.h
    @ONLY)

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

include(${CMAKE_CURRENT_SOURCE_DIR}/../core/fec/IsalEc.cmake)
winject_add_isal_ec()

include(${CMAKE_CURRENT_SOURCE_DIR}/../core/CoreSources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../l3/L3Sources.cmake)
list(TRANSFORM WINJECT_CORE_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/../core/)
list(TRANSFORM WINJECT_L3_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/../l3/)

add_executable(winject-tests
    ReactorDrainTest.cpp
    MpduSlotTest.cpp
    MpduTest.cpp
    ConfigTest.cpp
    RsBlockErasureTest.cpp
    VendoredFecTest.cpp
    DatagramAirTest.cpp
    UpstreamTest.cpp
    AppShutdownTest.cpp
    NetUtilTest.cpp
    LCHeaderTest.cpp
    PhyAirtimeTest.cpp
    RxDemuxTest.cpp
    RadioMplaneClientTest.cpp
    ConsoleServiceTest.cpp
    ConsoleServiceSocketTest.cpp
    MplaneCorrelationTest.cpp
    MplaneErrStringTest.cpp
    RadioMplaneParseTest.cpp
    WifiFcsTest.cpp
    WifiUdpTest.cpp
    WifiQueueTest.cpp
    TxEventQueueTest.cpp
    VersionTest.cpp
    MetricsRegistryTest.cpp
    TxMuxTest.cpp
    OneShotTimerTest.cpp
    BwtestTest.cpp
    L3ApiTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/bwtest/Bwtest.cpp
    ${WINJECT_CORE_SOURCES}
    ${WINJECT_L3_SOURCES}
)

target_include_directories(winject-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../l3
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
