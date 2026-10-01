cmake_minimum_required(VERSION 3.16)
project(winject-manager CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/../BfcFetch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/fec/IsalEc.cmake)
winject_add_isal_ec()

add_executable(winject-manager
    Main.cpp
    App.cpp
    Config.cpp
    utils/Log.cpp
    utils/MetricsRegistry.cpp
    console/ConsoleClient.cpp
    console/MplaneCorrelation.cpp
    console/ConsoleService.cpp
    console/ConsoleParse.cpp
    frames/Frame.cpp
    frames/Mpdu.cpp
    fec/RsBlockErasure.cpp
    utils/NetUtil.cpp
    radio/WifiUdp.cpp
    radio/WifiFcs.cpp
    radio/RadioUpstreamTable.cpp
    radio/RadioMplaneParse.cpp
    radio/RadioManager.cpp
    radio/PhyAirtime.cpp
    radio/TxMux.cpp
    radio/RxDemux.cpp
    endpoint/Upstream.cpp
    endpoint/UpstreamStats.cpp
    endpoint/UdpEndpoint.cpp
)

target_include_directories(winject-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${bfc_SOURCE_DIR}/src
)

target_compile_options(winject-manager PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
    -Wno-missing-field-initializers
)

target_link_libraries(winject-manager PRIVATE pthread isal_ec)
