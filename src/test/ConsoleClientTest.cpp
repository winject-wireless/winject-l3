#include "Config.h"
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

}  // namespace

TEST(ConsoleClientTest, OutOfOrderRepliesCompleteCorrectCallbacks)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    Config cfg;
    cfg.device = "127.0.0.1";
    cfg.console_port = port;

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client.start_connect(cfg, &err));
    ASSERT_TRUE(client.finish_connect(&err));

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

    Config cfg;
    cfg.device = "127.0.0.1";
    cfg.console_port = port;

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client.start_connect(cfg, &err));
    ASSERT_TRUE(client.finish_connect(&err));

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
    cfg.device = "127.0.0.1";
    cfg.console_port = port;
    cfg.channel = 1;
    cfg.power_dbm = 20;
    cfg.modulation = "OFDM_24M";
    cfg.domain = 0x1234;

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client.start_connect(cfg, &err));
    ASSERT_TRUE(client.finish_connect(&err));

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

TEST(ConsoleClientTest, ApplyRadioEnosysStillPrograms)
{
    uint16_t port = 0;
    const int srv = open_udp_server(&port);
    ASSERT_GE(srv, 0);

    Config cfg;
    cfg.device = "127.0.0.1";
    cfg.console_port = port;
    cfg.channel = 1;
    cfg.power_dbm = 20;
    cfg.modulation = "OFDM_24M";
    cfg.domain = 0x1234;

    ConsoleClient client;
    std::string err;
    ASSERT_TRUE(client.start_connect(cfg, &err));
    ASSERT_TRUE(client.finish_connect(&err));

    std::atomic<bool> done{false};
    client.apply_radio(cfg, 0,
                       [&](MplaneResult r)
                       {
                           EXPECT_TRUE(r.ok);
                           done = true;
                       });

    uint8_t id = 0;
    std::string line;
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    EXPECT_EQ(line, "radio_caps_info");
    client.on_line("NOK:" + std::to_string(id) + " ENOSYS");

    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    client.on_line("OK:" + std::to_string(id) + " " + line);
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    client.on_line("OK:" + std::to_string(id));
    ASSERT_TRUE(recv_cmd(srv, &id, &line));
    client.on_line("OK:" + std::to_string(id));

    for (int i = 0; i < 50 && !done; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(done);
    close(srv);
}
