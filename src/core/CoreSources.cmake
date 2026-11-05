# winject-l3 core sources: the data path (frames, FEC, radio mux/demux,
# upstreams) and the radio m-plane client. Relative to this directory; shared
# by src/manager/CMakeLists.txt (Linux), src/test and the ESP-IDF component.
set(WINJECT_CORE_SOURCES
    utils/Log.cpp
    utils/MetricsRegistry.cpp
    utils/NetUtil.cpp
    utils/Version.cpp
    mplane/RadioMplaneClient.cpp
    mplane/MplaneCorrelation.cpp
    mplane/MplaneText.cpp
    frames/Frame.cpp
    frames/Mpdu.cpp
    fec/vendor/rs_block_erasure.cpp
    fec/vendor/rs_block_erasure_rx.cpp
    fec/vendor/rs_encode_scalar.cpp
    fec/FecTx.cpp
    fec/FecRx.cpp
    radio/WifiUdp.cpp
    radio/WifiQueue.cpp
    radio/WifiFcs.cpp
    radio/RadioUpstreamTable.cpp
    radio/RadioMplaneParse.cpp
    radio/RadioSupervisor.cpp
    radio/PhyAirtime.cpp
    radio/TxMux.cpp
    radio/RxDemux.cpp
    endpoint/UpstreamLink.cpp
    endpoint/UpstreamStats.cpp
    endpoint/DatagramAir.cpp
    endpoint/UDPUpstream.cpp
    endpoint/QueueUpstream.cpp
)
