#ifndef WINJECT_TEST_CONSOLE_TEST_HELPERS_H_
#define WINJECT_TEST_CONSOLE_TEST_HELPERS_H_

#include "console/ConsoleService.h"
#include "console/ManagerConsoleTypes.h"
#include "utils/IOReactor.h"

#include <arpa/inet.h>
#include <bfc/socket.hpp>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>

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

inline ManagerConsoleHandlers stub_console_handlers()
{
    ManagerConsoleHandlers h;
    h.add_upstream = [](const ManagerUpstreamView&, std::string*) -> bool
    {
        return false;
    };
    h.remove_upstream = [](uint8_t, std::string*) -> bool
    {
        return false;
    };
    h.list_upstream = [](const std::vector<uint8_t>&,
                         std::vector<ManagerUpstreamView>*,
                         std::string*) -> bool
    {
        return false;
    };
    h.update_upstream = [](const ManagerUpstreamUpdate&, ManagerUpstreamView*,
                           std::string*) -> bool
    {
        return false;
    };
    h.list_upstream_rx_stat = [](const std::vector<uint8_t>&,
                                 std::vector<ManagerUpstreamRxStatView>*,
                                 std::string*) -> bool
    {
        return false;
    };
    h.list_upstream_tx_stat = [](const std::vector<uint8_t>&,
                                 std::vector<ManagerUpstreamTxStatView>*,
                                 std::string*) -> bool
    {
        return false;
    };
    h.get_metrics = [](const std::vector<std::string>&,
                       std::vector<ManagerMetricView>*, std::string*) -> bool
    {
        return false;
    };
    h.radio_info = [](ManagerConsoleReply) {};
    h.radio_caps_info = [](ManagerConsoleReply) {};
    h.radio_stats = [](ManagerConsoleReply) {};
    h.radio_tx = [](const ManagerRadioUpdate&, ManagerConsoleReply) {};
    h.radio_reset = [](ManagerConsoleReply) {};
    h.config_slot = [](uint8_t, ManagerConsoleReply) {};
    h.radio_device = [](const ManagerRadioDeviceUpdate&, ManagerRadioDeviceView*,
                        std::string*) -> bool
    {
        return true;
    };
    return h;
}

// Runs ConsoleService on a background reactor thread until stop().
class ConsoleServiceHarness
{
public:
    ~ConsoleServiceHarness()
    {
        stop();
    }

    bool start(ManagerConsoleHandlers handlers, std::string* error)
    {
        stop();
        handlers_ = std::move(handlers);
        const uint16_t port = reserve_free_udp_port();
        if (port == 0)
        {
            if (error != nullptr)
            {
                *error = "no udp port";
            }
            return false;
        }
        console_in_ = {};
        console_in_.sin_family = AF_INET;
        console_in_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        console_in_.sin_port = htons(port);

        reactor_ = std::make_unique<IOReactor>();
        if (!service_.start(*reactor_, console_in_, handlers_, error))
        {
            reactor_.reset();
            return false;
        }
        reactor_->get_timer().wait_ms(10000,
                                      [&]()
                                      {
                                          reactor_->stop();
                                      });
        worker_ = std::thread(
            [&]()
            {
                reactor_->run();
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return true;
    }

    bool wait_for(const std::function<bool()>& cond, int timeout_ms = 500)
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

    void stop()
    {
        if (reactor_ != nullptr)
        {
            reactor_->stop();
        }
        if (worker_.joinable())
        {
            worker_.join();
        }
        service_.stop();
        reactor_.reset();
    }

    const sockaddr_in& console_in() const
    {
        return console_in_;
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
                           reinterpret_cast<const sockaddr*>(&console_in_),
                           sizeof(console_in_)) > 0;
    }

    bool recv_line(int fd, std::string* out, int timeout_ms = 500)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        char buf[4096];
        while (std::chrono::steady_clock::now() < deadline)
        {
            sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            const ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                                         reinterpret_cast<sockaddr*>(&from),
                                         &from_len);
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

private:
    ConsoleService service_;
    std::unique_ptr<IOReactor> reactor_;
    std::thread worker_;
    sockaddr_in console_in_{};
    ManagerConsoleHandlers handlers_;
};

}  // namespace test
}  // namespace winject

#endif  // WINJECT_TEST_CONSOLE_TEST_HELPERS_H_
