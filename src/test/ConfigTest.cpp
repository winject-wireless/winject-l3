#include "Config.h"

#include <arpa/inet.h>
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

using namespace winject;

namespace
{
std::string write_conf(const std::string& body)
{
    char path[] = "/tmp/winject-conf-XXXXXX";
    const int fd = mkstemp(path);
    EXPECT_GE(fd, 0);
    if (fd >= 0)
    {
        close(fd);
        std::ofstream out(path);
        out << body;
    }
    return path;
}
}  // namespace

TEST(ConfigTest, LoadsStandaloneUdp)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    EXPECT_EQ(ntohs(cfg.radio_device.mplane.sin_port), 22);
    EXPECT_EQ(ntohs(cfg.radio_device.dplane.sin_port), 9000);
    EXPECT_EQ(cfg.channel, 1);
    EXPECT_EQ(cfg.modulation, "OFDM_24M");
    EXPECT_EQ(cfg.power_dbm, 20);
    EXPECT_EQ(cfg.domain, 0x1234);
    ASSERT_EQ(cfg.upstreams.size(), 1u);
    EXPECT_EQ(upstream_mode(cfg.upstreams[0].endpoint),
              UpstreamMode::udp_static);
    const auto& ep = cfg.upstreams[0].endpoint;
    EXPECT_EQ(ep.rx, "0.0.0.0:22081");
    EXPECT_EQ(ep.tx, "127.0.0.1:21082");
    EXPECT_EQ(cfg.upstreams[0].bus_tx, 0xb2);
    EXPECT_EQ(cfg.upstreams[0].bus_rx, 0xa1);
    EXPECT_EQ(cfg.upstreams[0].fec_type, FecType::none);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsUdpGenericForwardingMode)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_GENERIC_FORWARDING
upstream-0.scheduler_budget = 100
upstream-0.tx_bus           = b2
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("UDP_STATIC_FORWARDING"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, PhyRateKbps)
{
    EXPECT_EQ(Config::phy_rate_kbps("OFDM_24M"), 24000u);
}

TEST(ConfigTest, RejectsMixedEndpointKeys)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.connect_address  = 127.0.0.1:9
upstream-0.bind_address     = 127.0.0.1:22081
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("does not match rx/tx"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, LoadsSameBusTxRx)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = b2
upstream-0.scheduler_budget = 100
upstream-0.connect_address  = 127.0.0.1:9
)");
    Config cfg;
    std::string err;
    EXPECT_TRUE(cfg.load(path, &err)) << err;
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsUnknownModulation)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = NOT_A_PHY
radio.power         = 20
winject.domain        = 1234
upstream.size = 0
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    std::remove(path.c_str());
}

// TX pacing follows the modulation. The old pacing keys are rejected.
TEST(ConfigTest, RejectsRemovedPacingKeys)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
winject.max_rate_kbps = 10000
winject.tx_gap_us     = 99999
winject.max_data_per_tick = 99
winject.tx_burst_size = 0
winject.tx_burst_interval_us = 5
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("winject.max_rate_kbps is removed"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsZeroDomain)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 0
upstream.size = 0
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    std::remove(path.c_str());
}

TEST(ConfigTest, LoadsRsBlockErasure)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 4096
upstream-0.bind_address     = 127.0.0.1:22081
upstream-0.fec.type         = RS_BLOCK_ERASURE
upstream-0.fec.k            = 10
upstream-0.fec.n            = 15
upstream-0.fec.timeout_ms   = 25
)");
    Config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    ASSERT_EQ(cfg.upstreams.size(), 1u);
    EXPECT_EQ(cfg.upstreams[0].fec_type, FecType::RsBlockErasure);
    EXPECT_EQ(cfg.upstreams[0].fec_k, 10);
    EXPECT_EQ(cfg.upstreams[0].fec_n, 15);
    EXPECT_EQ(cfg.upstreams[0].fec_timeout_ms, 25);
    std::remove(path.c_str());
}

TEST(ConfigTest, FecRejectsBadKn)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.bind_address     = 127.0.0.1:22081
upstream-0.fec.type         = RS_BLOCK_ERASURE
upstream-0.fec.k            = 15
upstream-0.fec.n            = 10
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("fec.k"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsUnknownUpstreamMode)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = TCP_SERVER_FORWARDING
upstream-0.tx_bus           = c3
upstream-0.rx_bus           = d4
upstream-0.scheduler_budget = 1024
upstream-0.bind_address     = 127.0.0.1:22022
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("mode"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, Channel14AcceptsDsss)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 14
radio.modulation    = DSS_1M_L
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    EXPECT_EQ(cfg.channel, 14);
    EXPECT_EQ(cfg.modulation, "DSS_1M_L");
    std::remove(path.c_str());
}

TEST(ConfigTest, CanonicalModulation)
{
    EXPECT_EQ(Config::canonical_modulation("OFDM_24M"), "OFDM_24M");
    EXPECT_EQ(Config::canonical_modulation("ofdm_24m"), "OFDM_24M");
    EXPECT_TRUE(Config::canonical_modulation("nope").empty());
    EXPECT_EQ(Config::phy_rate_kbps("ofdm_24m"), 24000u);
    EXPECT_TRUE(Config::modulation_ok_for_channel("CCK_11M_S", 14));
    EXPECT_TRUE(Config::modulation_ok_for_channel("DSS_1M_L", 14));
    EXPECT_FALSE(Config::modulation_ok_for_channel("OFDM_24M", 14));
    EXPECT_TRUE(Config::modulation_ok_for_channel("OFDM_24M", 1));
    EXPECT_FALSE(Config::modulation_ok_for_channel("nope", 1));
}

TEST(ConfigTest, RadioFcsValuesAndSkipConsoleRule)
{
    const std::string base = R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)";
    {
        const std::string path = write_conf(base + "radio.skip_console = 0\n");
        Config cfg;
        std::string err;
        EXPECT_FALSE(cfg.load(path, &err));
        EXPECT_NE(err.find("radio.skip_console is removed"), std::string::npos);
        std::remove(path.c_str());
    }
    {
        std::string body = base + "radio.fcs = signal\n";
        const std::string path = write_conf(body);
        Config cfg;
        std::string err;
        ASSERT_TRUE(cfg.load(path, &err)) << err;
        EXPECT_EQ(ntohs(cfg.radio_device.mplane.sin_port), 22);
        EXPECT_EQ(cfg.radio_device.fcs, RadioFcsConfig::signal);
        std::remove(path.c_str());
    }
    {
        std::string body = base + "radio.fcs = ACTUAL\n";
        const std::string path = write_conf(body);
        Config cfg;
        std::string err;
        ASSERT_TRUE(cfg.load(path, &err)) << err;
        EXPECT_EQ(cfg.radio_device.fcs, RadioFcsConfig::actual);
        std::remove(path.c_str());
    }
    {
        std::string body = base + "radio.fcs = bogus\n";
        const std::string path = write_conf(body);
        Config cfg;
        std::string err;
        EXPECT_FALSE(cfg.load(path, &err));
        std::remove(path.c_str());
    }
}

TEST(ConfigTest, Channel14RejectsOfdm)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 14
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("modulation"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, CcaOptional)
{
    const std::string base = R"(
radio.device        = 192.168.32.1
radio.console       = 22
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)";
    {
        const std::string path = write_conf(base);
        Config cfg;
        std::string err;
        ASSERT_TRUE(cfg.load(path, &err)) << err;
        EXPECT_FALSE(cfg.cca_explicit);
        std::remove(path.c_str());
    }
    {
        const std::string path = write_conf(base + "radio.cca = 0\n");
        Config cfg;
        std::string err;
        ASSERT_TRUE(cfg.load(path, &err)) << err;
        EXPECT_TRUE(cfg.cca_explicit);
        EXPECT_FALSE(cfg.cca);
        std::remove(path.c_str());
    }
    {
        const std::string path = write_conf(base + "radio.cca = true\n");
        Config cfg;
        std::string err;
        ASSERT_TRUE(cfg.load(path, &err)) << err;
        EXPECT_TRUE(cfg.cca_explicit);
        EXPECT_TRUE(cfg.cca);
        std::remove(path.c_str());
    }
    {
        const std::string path = write_conf(base + "radio.cca = maybe\n");
        Config cfg;
        std::string err;
        EXPECT_FALSE(cfg.load(path, &err));
        EXPECT_NE(err.find("radio.cca"), std::string::npos);
        std::remove(path.c_str());
    }
}

TEST(ConfigTest, LoadsWinjectConsoleIn)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
winject.console_in    = 127.0.0.1:2400
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    EXPECT_EQ(cfg.console_in, "127.0.0.1:2400");
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsManagerConsoleKey)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
manager.console_in    = 127.0.0.1:2400
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("moved to winject.console_in"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsLegacyBusKey)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.bus_tx           = b2
upstream-0.scheduler_budget = 100
upstream-0.tx               = 127.0.0.1:9
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("moved to upstream-0.tx_bus"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsInjectPortAlias)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.inject_port   = 9001
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.scheduler_budget = 100
upstream-0.tx               = 127.0.0.1:9
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("moved to radio.dplane_port"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsMovedRadioKeys)
{
    const std::string path = write_conf(R"(
winject.device        = 192.168.32.1
radio.channel         = 1
radio.modulation      = OFDM_24M
radio.power           = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("moved to radio.device"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsMissingDevice)
{
    const std::string path = write_conf(R"(
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("device"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, ValidateAcceptsFilledStruct)
{
    Config cfg;
    cfg.channel = 1;
    cfg.modulation = "OFDM_24M";
    cfg.power_dbm = 20;
    cfg.domain = 0x1234;
    cfg.radio_device.mplane.sin_family = AF_INET;
    ASSERT_EQ(
        inet_pton(AF_INET, "192.168.253.11", &cfg.radio_device.mplane.sin_addr),
        1);
    cfg.radio_device.mplane.sin_port = htons(22);
    cfg.radio_device.dplane.sin_family = AF_INET;
    ASSERT_EQ(
        inet_pton(AF_INET, "192.168.253.11", &cfg.radio_device.dplane.sin_addr),
        1);
    cfg.radio_device.dplane.sin_port = htons(9000);
    UpstreamConfig u;
    u.index = 0;
    u.mode = UpstreamMode::udp_server;
    u.bus_tx = 0xb2;
    u.bus_rx = 0xa1;
    u.scheduler_budget = 256;
    u.endpoint.rx = "127.0.0.1:29000";
    cfg.upstreams.push_back(u);
    std::string err;
    EXPECT_TRUE(cfg.validate(&err)) << err;

    cfg.domain = 0;
    EXPECT_FALSE(cfg.validate(&err));
    EXPECT_NE(err.find("domain"), std::string::npos);

    cfg.domain = 0x1234;
    cfg.modulation = "ofdm_24m";
    EXPECT_FALSE(cfg.validate(&err));
    EXPECT_NE(err.find("modulation"), std::string::npos);

    cfg.modulation = "OFDM_24M";
    const sockaddr_in mplane = cfg.radio_device.mplane;
    cfg.radio_device.mplane = {};
    EXPECT_FALSE(cfg.validate(&err));
    EXPECT_NE(err.find("mplane"), std::string::npos);

    cfg.radio_device.mplane = mplane;
    cfg.radio_device.dplane = {};
    EXPECT_FALSE(cfg.validate(&err));
    EXPECT_NE(err.find("dplane"), std::string::npos);
}

TEST(ConfigTest, LoadsQueueForwarding)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = QUEUE_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.queue_in         = video-to-radio
upstream-0.queue_out        = radio-to-video
upstream-0.reactor          = host-reactor
upstream-0.fec.type         = RS_BLOCK_ERASURE
upstream-0.fec.k            = 10
upstream-0.fec.n            = 12
)");
    Config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    ASSERT_EQ(cfg.upstreams.size(), 1u);
    EXPECT_EQ(cfg.upstreams[0].mode, UpstreamMode::queue);
    EXPECT_EQ(cfg.upstreams[0].queue_in, "video-to-radio");
    EXPECT_EQ(cfg.upstreams[0].queue_out, "radio-to-video");
    EXPECT_EQ(cfg.upstreams[0].reactor, "host-reactor");
    EXPECT_EQ(cfg.upstreams[0].fec_type, FecType::RsBlockErasure);
    EXPECT_EQ(cfg.upstreams[0].fec_k, 10);
    EXPECT_EQ(cfg.upstreams[0].fec_n, 12);
    EXPECT_TRUE(cfg.upstreams[0].endpoint.rx.empty());
    EXPECT_TRUE(cfg.upstreams[0].endpoint.tx.empty());
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsQueueForwardingWithoutAPair)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = QUEUE_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.scheduler_budget = 100
upstream-0.queue_in         = video-to-radio
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("queue"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, RejectsSharedQueueNames)
{
    const std::string path = write_conf(R"(
radio.device        = 192.168.32.1
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 2
upstream-0.mode             = QUEUE_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.scheduler_budget = 100
upstream-0.queue_in         = shared
upstream-0.queue_out        = radio-to-video
upstream-0.reactor          = host-reactor
upstream-1.mode             = QUEUE_FORWARDING
upstream-1.tx_bus           = c3
upstream-1.scheduler_budget = 100
upstream-1.queue_in         = other-to-radio
upstream-1.queue_out        = shared
upstream-1.reactor          = host-reactor
)");
    Config cfg;
    std::string err;
    EXPECT_FALSE(cfg.load(path, &err));
    EXPECT_NE(err.find("queue name"), std::string::npos);
    std::remove(path.c_str());
}

TEST(ConfigTest, LoadsRadioDeviceFromLegacyKeys)
{
    const std::string path = write_conf(R"(
radio.device        = 127.0.0.1
radio.console       = 2201
radio.dplane_port   = 9001
radio.channel       = 1
radio.modulation    = OFDM_24M
radio.power         = 20
winject.domain        = 1234
upstream.size = 1
upstream-0.mode             = UDP_STATIC_FORWARDING
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1
upstream-0.scheduler_budget = 100
upstream-0.rx               = 0.0.0.0:22081
upstream-0.tx               = 127.0.0.1:21082
)");
    Config cfg;
    std::string err;
    ASSERT_TRUE(cfg.load(path, &err)) << err;
    EXPECT_EQ(ntohs(cfg.radio_device.mplane.sin_port), 2201u);
    EXPECT_EQ(ntohs(cfg.radio_device.dplane.sin_port), 9001u);
    std::remove(path.c_str());
}
