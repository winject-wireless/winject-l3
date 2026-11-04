#ifndef WINJECT_TEST_CONSOLE_TEST_HELPERS_H_
#define WINJECT_TEST_CONSOLE_TEST_HELPERS_H_

#include "Config.h"
#include "NodeCore.h"
#include "WinjectBuildVersion.h"
#include "mplane/MplaneCorrelation.h"
#include "utils/IOReactor.h"
#include "winject_l3/Console.h"
#include "winject_l3/Node.h"

#include <arpa/inet.h>
#include <atomic>
#include <bfc/socket.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace winject
{
namespace test
{

inline uint16_t reserve_free_udp_port()
{
    bfc::socket probe(bfc::create_udp4());
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (probe.bind(addr) < 0)
    {
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(probe.fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0)
    {
        return 0;
    }
    return ntohs(addr.sin_port);
}

inline sockaddr_in loopback(uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

inline bool run_reactor_with_watchdog(IOReactor& reactor,
                                      const std::function<void()>& unblock)
{
    reactor.get_timer().wait_ms(50,
                                [&]()
                                {
                                    reactor.stop();
                                });
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    std::thread worker(
        [&]()
        {
            reactor.run();
            done.set_value();
        });
    const bool ok =
        finished.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!ok)
    {
        unblock();
    }
    worker.join();
    return ok;
}

inline bool wait_until(const std::function<bool()>& cond, int timeout_ms = 1000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cond())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return cond();
}

// A radio on loopback. Its m-plane answers what the node sends on connect
// and while up, as the firmwares do. A test can hold a command to answer it
// itself. The d-plane socket takes the node's registrations and frames.
class FakeRadio
{
public:
    FakeRadio()
        : mplane_sock(bfc::create_udp4()), dplane_sock(bfc::create_udp4())
    {
        bind_any(mplane_sock, &mplane_addr);
        bind_any(dplane_sock, &dplane_addr);
        worker = std::thread(
            [this]()
            {
                serve();
            });
    }

    ~FakeRadio()
    {
        running = false;
        worker.join();
    }

    const sockaddr_in& mplane() const
    {
        return mplane_addr;
    }
    const sockaddr_in& dplane() const
    {
        return dplane_addr;
    }
    int dplane_fd() const
    {
        return dplane_sock.fd();
    }

    // Requests whose line starts with prefix are held, not answered.
    void hold(const std::string& prefix)
    {
        std::lock_guard<std::mutex> lock(mu);
        held[prefix];
    }
    size_t held_count(const std::string& prefix)
    {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = held.find(prefix);
        return it == held.end() ? 0 : it->second.size();
    }
    bool wait_held(const std::string& prefix, size_t n, int timeout_ms = 1000)
    {
        return wait_until(
            [&]()
            {
                return held_count(prefix) >= n;
            },
            timeout_ms);
    }
    // Answers the i-th held request for prefix: "OK:<id> <body>" lines, one
    // per '\n' in body, or "NOK:<id> <body>".
    void answer(const std::string& prefix, size_t i, bool ok,
                const std::string& body)
    {
        Request req;
        {
            std::lock_guard<std::mutex> lock(mu);
            req = held.at(prefix).at(i);
        }
        send_reply(req, ok, body);
    }
    // How many requests started with prefix.
    size_t seen(const std::string& prefix)
    {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const auto& line : log)
        {
            if (line.rfind(prefix, 0) == 0)
            {
                ++n;
            }
        }
        return n;
    }
    // The answer to radio_caps_info; empty answers NOK ENOSYS (an old
    // radio).
    void set_caps(const std::string& fcs)
    {
        std::lock_guard<std::mutex> lock(mu);
        caps = fcs;
    }

    static std::string tx_info_line(uint64_t n)
    {
        const std::string v = std::to_string(n);
        return "tx_info tx_queue_sz=" + v + " in_flight=" + v +
               " dropped_invalid_frame=" + v + " dropped_tx_queue=" + v +
               " dropped_wifi=" + v + " ether_pkt=" + v + " air_pkt=" + v +
               " ts=" + v;
    }
    static std::string rx_info_line(uint64_t n)
    {
        const std::string v = std::to_string(n);
        return "rx_info rx_queue_sz=" + v + " dropped_filter_mismatched=" + v +
               " dropped_rx_queue=" + v + " dropped_no_peer=" + v +
               " dropped_send_failed=" + v + " ether_pkt=" + v +
               " air_pkt=" + v + " ts=" + v;
    }

private:
    struct Request
    {
        uint8_t id = 0;
        sockaddr_in peer{};
    };

    static void bind_any(bfc::socket& sock, sockaddr_in* out)
    {
        sockaddr_in addr = loopback(0);
        sock.bind(addr);
        socklen_t len = sizeof(addr);
        getsockname(sock.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
        *out = addr;
    }

    void send_reply(const Request& req, bool ok, const std::string& body)
    {
        const std::string tag = (ok ? "OK:" : "NOK:") + std::to_string(req.id);
        std::string wire;
        size_t off = 0;
        do
        {
            const size_t nl = body.find('\n', off);
            const std::string line = body.substr(
                off, nl == std::string::npos ? std::string::npos : nl - off);
            wire += line.empty() ? tag : tag + " " + line;
            wire += '\n';
            off = nl == std::string::npos ? body.size() : nl + 1;
        } while (off < body.size());
        ::sendto(mplane_sock.fd(), wire.data(), wire.size(), 0,
                 reinterpret_cast<const sockaddr*>(&req.peer),
                 sizeof(req.peer));
    }

    std::string phy_line()
    {
        return "radio_tx channel=" + std::to_string(channel) +
               " tx_power=" + std::to_string(power) +
               " modulation=" + modulation + " cca=false";
    }

    static std::string arg(const std::string& line, const std::string& key)
    {
        const size_t pos = line.find(" " + key + "=");
        if (pos == std::string::npos)
        {
            return {};
        }
        const size_t start = pos + key.size() + 2;
        const size_t end = line.find(' ', start);
        return line.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
    }

    void on_request(const Request& req, const std::string& line)
    {
        std::unique_lock<std::mutex> lock(mu);
        log.push_back(line);
        for (auto& entry : held)
        {
            if (line.rfind(entry.first, 0) == 0)
            {
                entry.second.push_back(req);
                return;
            }
        }
        if (line == "version")
        {
            lock.unlock();
            send_reply(req, true,
                       std::string("version ver=") + WINJECT_VERSION_STRING +
                           " proto=1.0");
        }
        else if (line == "radio_caps_info")
        {
            const std::string fcs = caps;
            lock.unlock();
            if (fcs.empty())
            {
                send_reply(req, false, "ENOSYS");
            }
            else
            {
                send_reply(req, true, "radio_caps_info fcs=" + fcs);
            }
        }
        else if (line.rfind("radio_tx ", 0) == 0)
        {
            const std::string ch = arg(line, "channel");
            const std::string pw = arg(line, "tx_power");
            const std::string mod = arg(line, "modulation");
            if (!ch.empty())
            {
                channel = std::stoi(ch);
            }
            if (!pw.empty())
            {
                power = std::stoi(pw);
            }
            if (!mod.empty())
            {
                modulation = mod;
            }
            const std::string reply = phy_line();
            lock.unlock();
            send_reply(req, true, reply);
        }
        else if (line == "radio_tx_info")
        {
            const std::string reply = phy_line() + "\nradio_rx rssi=-40";
            lock.unlock();
            send_reply(req, true, reply);
        }
        else if (line.rfind("radio_tx_cal", 0) == 0)
        {
            // A radio-specific command (rmp passthrough).
            lock.unlock();
            send_reply(req, true,
                       "radio_tx_cal group=dsss default=false "
                       "points=8:-28.00,84:15.60");
        }
        else if (line == "ping")
        {
            lock.unlock();
            send_reply(req, true, "pong");
        }
        else if (line == "tx_info")
        {
            lock.unlock();
            send_reply(req, true, tx_info_line(0));
        }
        else if (line == "rx_info")
        {
            lock.unlock();
            send_reply(req, true, rx_info_line(0));
        }
        else if (line.rfind("rx_filter_addr3", 0) == 0 ||
                 line.rfind("save ", 0) == 0 || line.rfind("load ", 0) == 0 ||
                 line == "reset")
        {
            lock.unlock();
            send_reply(req, true, "");
        }
        else
        {
            lock.unlock();
            send_reply(req, false, "ENOSYS");
        }
    }

    void serve()
    {
        char buf[2048];
        while (running)
        {
            pollfd fds[2] = {{mplane_sock.fd(), POLLIN, 0},
                             {dplane_sock.fd(), POLLIN, 0}};
            if (::poll(fds, 2, 10) <= 0)
            {
                continue;
            }
            if (fds[1].revents & POLLIN)
            {
                ::recv(dplane_sock.fd(), buf, sizeof(buf), MSG_DONTWAIT);
            }
            if (!(fds[0].revents & POLLIN))
            {
                continue;
            }
            Request req;
            socklen_t len = sizeof(req.peer);
            const ssize_t n =
                ::recvfrom(mplane_sock.fd(), buf, sizeof(buf) - 1, MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&req.peer), &len);
            if (n <= 0)
            {
                continue;
            }
            buf[n] = '\0';
            std::string text(buf);
            while (!text.empty() &&
                   (text.back() == '\n' || text.back() == '\r'))
            {
                text.pop_back();
            }
            const char* body = nullptr;
            bool malformed = false;
            if (!parse_mplane_cmd(text.c_str(), &req.id, &body, &malformed) ||
                malformed)
            {
                continue;
            }
            on_request(req, body);
        }
    }

    bfc::socket mplane_sock;
    bfc::socket dplane_sock;
    sockaddr_in mplane_addr{};
    sockaddr_in dplane_addr{};
    std::atomic<bool> running{true};
    std::mutex mu;
    std::map<std::string, std::vector<Request>> held;
    std::vector<std::string> log;
    std::string caps = "SIGNAL";
    int channel = 1;
    int power = 20;
    std::string modulation = "OFDM_24M";
    std::thread worker;
};

// A node config for a FakeRadio, with one UDP upstream (Config needs one).
inline Config config_for(const FakeRadio& radio)
{
    Config cfg;
    cfg.domain = 0x1234;
    cfg.modulation = "OFDM_24M";
    cfg.radio_device.mplane = radio.mplane();
    cfg.radio_device.dplane = radio.dplane();
    cfg.radio_device.fcs = RadioFcsConfig::signal;
    UpstreamConfig up;
    up.index = 4;
    up.mode = UpstreamMode::udp_client;
    up.endpoint.tx = "127.0.0.1:9";
    up.bus_tx = 1;
    up.bus_rx = 2;
    cfg.upstreams.push_back(up);
    return cfg;
}

// Runs one node on its own core (not the process's), with run() on a
// thread of its own that becomes the node's callback thread.
class TestNode
{
public:
    TestNode()
        : core(winject_l3::detail::NodeCore::create()),
          node(winject_l3::detail::HandleAccess::node(core))
    {
    }
    ~TestNode()
    {
        stop();
    }

    // Returns once started(node) ran, or run() returned early.
    bool start(const Config& cfg,
               std::function<void(const winject_l3::Node&)> started = {})
    {
        auto ready = std::make_shared<std::promise<void>>();
        auto is_ready = ready->get_future();
        auto once = std::make_shared<std::once_flag>();
        auto signal = [ready, once]()
        {
            std::call_once(*once,
                           [&]()
                           {
                               ready->set_value();
                           });
        };
        result = std::async(std::launch::async,
                            [this, cfg, started, signal]()
                            {
                                const int rc = winject_l3::detail::run_node(
                                    core, cfg,
                                    [started, signal](const winject_l3::Node& n)
                                    {
                                        if (started)
                                        {
                                            started(n);
                                        }
                                        signal();
                                    },
                                    {});
                                signal();
                                return rc;
                            });
        return is_ready.wait_for(std::chrono::seconds(5)) ==
                   std::future_status::ready &&
               node.running();
    }

    bool radio_up(int timeout_ms = 2000)
    {
        return wait_until(
            [this]()
            {
                auto radio = node.radios().get();
                if (!radio.ok())
                {
                    return false;
                }
                auto state = radio.value.state();
                return state.ok() && state.value == winject_l3::RadioState::up;
            },
            timeout_ms);
    }

    // Stops the node; returns run()'s result, or -1 if it did not return.
    int stop()
    {
        if (!result.valid())
        {
            return stopped_rc;
        }
        core->request_stop();
        if (result.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready)
        {
            stopped_rc = -1;
            return stopped_rc;
        }
        stopped_rc = result.get();
        return stopped_rc;
    }

    std::shared_ptr<winject_l3::detail::NodeCore> core;
    winject_l3::Node node;

private:
    std::future<int> result;
    int stopped_rc = 0;
};

// A console reactor registered under a unique name, on its own thread.
class ConsoleReactor
{
public:
    ConsoleReactor() : name("test-console-" + std::to_string(next_id()))
    {
        reactor = std::make_shared<winject_l3::console::Reactor>();
        winject_l3::console::ReactorRegistry::get_instance().add(name, reactor);
        std::promise<void> running;
        auto is_running = running.get_future();
        reactor->wake_up(
            [&running]()
            {
                running.set_value();
            });
        worker = std::thread(
            [r = reactor]()
            {
                r->run();
            });
        is_running.wait();
    }
    ~ConsoleReactor()
    {
        reactor->stop();
        worker.join();
        winject_l3::console::ReactorRegistry::get_instance().rem(name);
    }

    // Runs fn on the reactor's thread and waits for it.
    bool on_reactor(const std::function<void()>& fn, int timeout_ms = 500)
    {
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        reactor->wake_up(
            [fn, done]()
            {
                fn();
                done->set_value();
            });
        return finished.wait_for(std::chrono::milliseconds(timeout_ms)) ==
               std::future_status::ready;
    }

    const std::string name;
    std::shared_ptr<winject_l3::console::Reactor> reactor;

private:
    static int next_id()
    {
        static std::atomic<int> id{0};
        return ++id;
    }
    std::thread worker;
};

// The UDP console of a real node, on a FakeRadio: what the manager binary
// runs.
class ConsoleServiceHarness
{
public:
    ~ConsoleServiceHarness()
    {
        stop();
    }

    // Starts the node and its console; with wait_up, also waits until the
    // radio m-plane is up.
    bool start(std::string* error, bool wait_up = true)
    {
        const uint16_t port = reserve_free_udp_port();
        if (port == 0)
        {
            *error = "no udp port";
            return false;
        }
        console_bind = loopback(port);
        Config cfg = config_for(radio);
        cfg.console_in = "127.0.0.1:" + std::to_string(port);
        winject_l3::Status console_status;
        console_status.error = "not started";
        if (!node.start(cfg,
                        [&](const winject_l3::Node& n)
                        {
                            console_status =
                                service.start(n, cfg.console_in, reactor.name);
                        }))
        {
            *error = "node did not start";
            return false;
        }
        if (!console_status.ok())
        {
            *error = console_status.error;
            return false;
        }
        if (wait_up && !node.radio_up())
        {
            *error = "radio not up";
            return false;
        }
        return true;
    }

    void stop()
    {
        service.stop();
        node.stop();
    }
    // Stops only the console: the node keeps running.
    void stop_console()
    {
        service.stop();
    }

    const sockaddr_in& console_in() const
    {
        return console_bind;
    }

    bool send_line(bfc::socket& client, const char* line)
    {
        std::string wire = line;
        if (wire.empty() || wire.back() != '\n')
        {
            wire += '\n';
        }
        const bfc::const_buffer_view view(
            reinterpret_cast<const std::byte*>(wire.data()), wire.size());
        return client.send(view, 0,
                           reinterpret_cast<const sockaddr*>(&console_bind),
                           sizeof(console_bind)) > 0;
    }

    bool recv_line(int fd, std::string* out, int timeout_ms = 1000)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        char buf[4096];
        while (std::chrono::steady_clock::now() < deadline)
        {
            sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            const ssize_t n =
                ::recvfrom(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n > 0)
            {
                buf[n] = '\0';
                *out = buf;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    bool exchange(bfc::socket& client, const char* line, std::string* reply)
    {
        if (!send_line(client, line))
        {
            return false;
        }
        return recv_line(client.fd(), reply);
    }

    FakeRadio radio;
    TestNode node;
    ConsoleReactor reactor;
    winject_l3::console::Service service;

private:
    sockaddr_in console_bind{};
};

}  // namespace test
}  // namespace winject

#endif  // WINJECT_TEST_CONSOLE_TEST_HELPERS_H_
