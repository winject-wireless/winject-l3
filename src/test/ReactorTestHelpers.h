#ifndef WINJECT_TEST_REACTOR_TEST_HELPERS_H_
#define WINJECT_TEST_REACTOR_TEST_HELPERS_H_

#include "utils/IOReactor.h"

#include <bfc/socket.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <netinet/in.h>
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

}  // namespace test
}  // namespace winject

#endif  // WINJECT_TEST_REACTOR_TEST_HELPERS_H_
