#include "Config.h"
#include "WinjectBuildVersion.h"
#include "console/ConsoleClient.h"
#include "console/MplaneCorrelation.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
using winject::Config;
using winject::ConsoleClient;
using winject::MplaneResult;
int open_udp_server(uint16_t* port_out)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    *port_out = ntohs(addr.sin_port);
    return fd;
}

bool recv_cmd(int fd, uint8_t* id_out, std::string* line_out)
{
    char buf[512];
    const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        return false;
    }
    buf[n] = '\0';
    const std::string wire(buf);
    if (wire.rfind("cmd:", 0) != 0)
    {
        return false;
    }
    const size_t sp = wire.find(' ');
    if (sp == std::string::npos || sp <= 4)
    {
        return false;
    }
    *id_out = static_cast<uint8_t>(std::stoi(wire.substr(4, sp - 4)));
    *line_out = wire.substr(sp + 1);
    while (!line_out->empty() &&
           (line_out->back() == '\n' || line_out->back() == '\r'))
    {
        line_out->pop_back();
    }
    return true;
}

sockaddr_in loopback_mplane(uint16_t port)
{
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

std::string version_ok_payload()
{
    return std::string("version ver=") + WINJECT_VERSION_STRING + " proto=1.0";
}

bool client_connect(ConsoleClient& client, uint16_t port, std::string* err)
{
    return client.start_connect(loopback_mplane(port), err) &&
           client.finish_connect(err);
}

}  // namespace

TEST(ConsoleClientTest, OutOfOrderRepliesCompleteCorrectCallbacks)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    std::atomic<int> done_count{0};
    std::string first_payload;
    std::string second_payload;

    ASSERT_TRUE(client.request("ping",
                               [&](MplaneResult r)
                               {
                                   EXPECT_TRUE(r.ok);
                                   first_payload = r.payload;
                                   ++done_count;
                               }));
    ASSERT_TRUE(client.request("version",
                               [&](MplaneResult r)
                               {
                                   EXPECT_TRUE(r.ok);
                                   second_payload = r.payload;
                                   ++done_count;
                               }));

    uint8_t id1 = 0;
    uint8_t id2 = 0;
    std::string line1;
    std::string line2;
    ASSERT_TRUE(recv_cmd(srv, &id1, &line1));
    ASSERT_TRUE(recv_cmd(srv, &id2, &line2));
    EXPECT_EQ(line1, "ping");
    EXPECT_EQ(line2, "version");

    client.on_line("OK:" + std::to_string(id2) + " v1");
    client.on_line("OK:" + std::to_string(id1) + " pong");

    for (int i = 0; i < 50 && done_count < 2; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(done_count, 2);
    EXPECT_EQ(first_payload, "pong");
    EXPECT_EQ(second_payload, "v1");
    close(srv);
}

TEST(ConsoleClientTest, UnknownReplyIdIsIgnored)
{
    ConsoleClient client;
    client.on_line("OK:42 orphan");
    SUCCEED();
}

TEST(ConsoleClientTest, DeadlineCompletesWithTimeout)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    std::atomic<bool> timed_out{false};
    ASSERT_TRUE(client.request("ping", std::chrono::milliseconds(5),
                               [&](MplaneResult r)
                               {
                                   EXPECT_FALSE(r.ok);
                                   EXPECT_EQ(r.error, "timeout");
                                   timed_out = true;
                               }));

    char buf[256];
    recv(srv, buf, sizeof(buf), 0);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (!timed_out && std::chrono::steady_clock::now() < deadline)
    {
        client.poll_deadlines(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(timed_out);
    close(srv);
}

TEST(ConsoleClientTest, ApplyRadioQueriesCapsFirst)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    Config cfg;
    cfg.channel = 1;
    cfg.power_dbm = 20;
    cfg.modulation = "OFDM_24M";
    cfg.domain = 0x1234;

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    std::atomic<bool> done{false};
    std::atomic<bool> caps_cb{false};
    client.apply_radio(
        cfg, 0,
        [&](MplaneResult r)
        {
            EXPECT_TRUE(r.ok);
            done = true;
        },
        [&](const MplaneResult& caps)
        {
            caps_cb = true;
            EXPECT_TRUE(caps.ok);
        });

    uint8_t id = 0;
    std::string line;
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "version");
    client.on_line("OK:" + std::to_string(id) + " " + version_ok_payload());

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "radio_caps_info");
    client.on_line("OK:" + std::to_string(id) + " radio_caps_info fcs=SIGNAL");

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_TRUE(line.rfind("radio_tx ", 0) == 0);
    client.on_line("OK:" + std::to_string(id) + " " + line);

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_TRUE(line.rfind("rx_filter_addr3 ", 0) == 0);
    client.on_line("OK:" + std::to_string(id));

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "save 0");
    client.on_line("OK:" + std::to_string(id));

    for (int i = 0; i < 50 && !done; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(caps_cb);
    EXPECT_TRUE(done);
    close(srv);
}

TEST(ConsoleClientTest, ApplyRejectsMissingVersion)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    Config cfg;
    cfg.channel = 1;
    cfg.power_dbm = 20;
    cfg.modulation = "OFDM_24M";
    cfg.domain = 0x1234;

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    std::atomic<bool> done{false};
    client.apply_radio(cfg, 0,
                       [&](MplaneResult r)
                       {
                           EXPECT_FALSE(r.ok);
                           EXPECT_EQ(r.error, "EPROTO");
                           done = true;
                       });

    uint8_t id = 0;
    std::string line;
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "version");
    client.on_line("NOK:" + std::to_string(id) + " ENOSYS");

    for (int i = 0; i < 50 && !done; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(done);
    close(srv);
}

TEST(ConsoleClientTest, ApplyRadioSendsCcaWhenConfigured)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    Config cfg;
    cfg.channel = 1;
    cfg.power_dbm = 20;
    cfg.modulation = "OFDM_24M";
    cfg.domain = 0x1234;
    cfg.cca = false;
    cfg.cca_explicit = true;

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    client.apply_radio(cfg, 0, [](MplaneResult) {});

    uint8_t id = 0;
    std::string line;
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "version");
    client.on_line("OK:" + std::to_string(id) + " " + version_ok_payload());

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "radio_caps_info");
    client.on_line("OK:" + std::to_string(id) + " radio_caps_info fcs=SIGNAL");

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "radio_tx channel=1 tx_power=20 modulation=OFDM_24M cca=0");
    client.close();
    close(srv);
}

TEST(ConsoleClientTest, QueryRadioCountersJoinsTxAndRx)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    bool done = false;
    MplaneResult result;
    client.query_radio_counters(
        [&](MplaneResult r)
        {
            result = std::move(r);
            done = true;
        });

    uint8_t id = 0;
    std::string line;
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "tx_info");
    client.on_line("OK:" + std::to_string(id) +
                   " tx_info ether_pkt=10 air_pkt=9 ts=1");
    EXPECT_FALSE(done);

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "rx_info");
    client.on_line("OK:" + std::to_string(id) +
                   " rx_info ether_pkt=8 air_pkt=9 ts=2");

    ASSERT_TRUE(done);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(result.body_lines.size(), 2u);
    EXPECT_EQ(result.body_lines[0], "tx_info ether_pkt=10 air_pkt=9 ts=1");
    EXPECT_EQ(result.body_lines[1], "rx_info ether_pkt=8 air_pkt=9 ts=2");
    close(srv);
}

TEST(ConsoleClientTest, QueryRadioCountersStopsOnTxError)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    bool done = false;
    MplaneResult result;
    client.query_radio_counters(
        [&](MplaneResult r)
        {
            result = std::move(r);
            done = true;
        });

    uint8_t id = 0;
    std::string line;
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    client.on_line("NOK:" + std::to_string(id) + " ENOSYS");

    ASSERT_TRUE(done);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("ENOSYS"), std::string::npos);
    close(srv);
}

TEST(ConsoleClientTest, MultiLineReplyCompletesOnLastLine)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    bool done = false;
    MplaneResult result;
    client.query_radio_info(
        [&](MplaneResult r)
        {
            result = std::move(r);
            done = true;
        });

    uint8_t id = 0;
    std::string line;
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "radio_tx_info");
    const std::string sid = std::to_string(id);
    client.on_lines({"OK:" + sid +
                         " radio_tx channel=1 tx_power=20 "
                         "modulation=OFDM_24M cca=false",
                     "OK:" + sid + " radio_rx rssi=-30"});

    ASSERT_TRUE(done);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(result.body_lines.size(), 2u);
    EXPECT_EQ(result.body_lines[1], "radio_rx rssi=-30");
    close(srv);
}

// A failure callback that drops the console (as RadioManager's ping handler
// does) re-enters cancel_pending(). Each callback must run exactly once.
TEST(ConsoleClientTest, CancelFromCallbackRunsEachOnce)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);
    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    int ping_calls = 0;
    int other_calls = 0;
    ASSERT_TRUE(client.request("ping",
                               [&](MplaneResult r)
                               {
                                   EXPECT_FALSE(r.ok);
                                   ++ping_calls;
                                   client.cancel_pending();
                               }));
    ASSERT_TRUE(client.request("radio_tx_info",
                               [&](MplaneResult r)
                               {
                                   EXPECT_FALSE(r.ok);
                                   ++other_calls;
                                   client.cancel_pending();
                               }));
    client.cancel_pending();
    EXPECT_EQ(ping_calls, 1);
    EXPECT_EQ(other_calls, 1);
    close(srv);
}

// A timeout callback that drops the console must not leave poll_deadlines()
// walking a cleared map; the other expired request still completes once.
TEST(ConsoleClientTest, TimeoutCallbackMayCancelPending)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);
    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client_connect(client, port, &err));

    std::vector<std::string> errors;
    for (const char* cmd : {"ping", "radio_tx_info", "save 0"})
    {
        ASSERT_TRUE(client.request(cmd, std::chrono::milliseconds(1),
                                   [&](MplaneResult r)
                                   {
                                       errors.push_back(r.error);
                                       client.cancel_pending();
                                   }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    client.poll_deadlines(std::chrono::steady_clock::now());
    ASSERT_EQ(errors.size(), 3u);
    EXPECT_EQ(errors[0], "timeout");
    client.poll_deadlines(std::chrono::steady_clock::now());
    EXPECT_EQ(errors.size(), 3u);
    close(srv);
}
