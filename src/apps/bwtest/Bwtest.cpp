#include "apps/bwtest/Bwtest.h"

#include "utils/NetUtil.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace app_bwtest
{

namespace
{

std::atomic<bool> g_stop{false};

// Sleep between pacing checks and receive polls. Pacing catches up from the
// clock, so a longer tick (ESP-IDF rounds up to FreeRTOS ticks) only makes
// the bursts bigger, not the rate lower.
constexpr auto k_tick = std::chrono::milliseconds(1);

}  // namespace

Bwtest::Bwtest(std::shared_ptr<winject::UpstreamQueue> upstream,
               size_t payload_bytes)
    : upstream_(std::move(upstream)), payload_bytes_(payload_bytes)
{
    clear();
}

Bwtest::~Bwtest()
{
    stop_.store(true);
    std::lock_guard<std::mutex> lock(send_mu_);
    if (send_thread_.joinable())
    {
        send_thread_.join();
    }
}

bool Bwtest::start_send(uint32_t offer_kbps, uint32_t duration_ms)
{
    if (offer_kbps == 0 || duration_ms == 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mu_);
    if (sending_.load())
    {
        return false;
    }
    if (send_thread_.joinable())
    {
        send_thread_.join();
    }
    sending_.store(true);
    send_thread_ = std::thread(
        [this, offer_kbps, duration_ms]
        {
            run_send(offer_kbps, duration_ms);
            sending_.store(false);
        });
    return true;
}

bool Bwtest::sending() const
{
    return sending_.load();
}

void Bwtest::run_send(uint32_t offer_kbps, uint32_t duration_ms)
{
    using clock = std::chrono::steady_clock;
    std::vector<uint8_t> payload(payload_bytes_, 0);
    const auto start = clock::now();
    const auto end = start + std::chrono::milliseconds(duration_ms);
    // Bits per µs = kbps / 1000, so due datagrams after t µs are
    // t * kbps / (8000 * payload).
    const uint64_t divisor = 8000ull * payload_bytes_;
    uint64_t sent = 0;
    for (;;)
    {
        const auto now = std::min(clock::now(), end);
        const uint64_t elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - start)
                .count();
        const uint64_t due = elapsed_us * offer_kbps / divisor;
        for (; sent < due; ++sent)
        {
            const uint32_t seq = send_seq_++;
            for (size_t i = 0; i < 4 && i < payload.size(); ++i)
            {
                payload[i] = static_cast<uint8_t>(seq >> (8 * i));
            }
            if (!upstream_->try_send(payload.data(), payload.size()))
            {
                snd_reject_.fetch_add(1);
            }
            snd_pkt_.fetch_add(1);
            snd_byt_.fetch_add(payload.size());
        }
        if (now >= end || stop_.load())
        {
            return;
        }
        std::this_thread::sleep_for(k_tick);
    }
}

size_t Bwtest::poll_receive()
{
    size_t count = 0;
    while (auto packet = upstream_->receive())
    {
        rcv_pkt_.fetch_add(1);
        rcv_byt_.fetch_add(packet->size());
        ++count;
    }
    return count;
}

uint64_t Bwtest::evictions() const
{
    return upstream_->ingress_evictions.load();
}

Stats Bwtest::stats() const
{
    Stats s;
    s.snd_pkt = snd_pkt_.load();
    s.snd_byt = snd_byt_.load();
    s.snd_drop = snd_reject_.load() + (evictions() - eviction_base_.load());
    s.rcv_pkt = rcv_pkt_.load();
    s.rcv_byt = rcv_byt_.load();
    return s;
}

void Bwtest::clear()
{
    snd_pkt_.store(0);
    snd_byt_.store(0);
    snd_reject_.store(0);
    eviction_base_.store(evictions());
    rcv_pkt_.store(0);
    rcv_byt_.store(0);
}

bool Bwtest::add_command(const std::string& name, Command command)
{
    if (name == "test_send" || name == "stats" || name == "clr" || !command)
    {
        return false;
    }
    commands_[name] = std::move(command);
    return true;
}

std::string Bwtest::handle_command(const std::string& line)
{
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "test_send")
    {
        uint64_t kbps = 0;
        uint64_t ms = 0;
        std::string extra;
        if (!(in >> kbps >> ms) || (in >> extra) || kbps == 0 || ms == 0 ||
            kbps > UINT32_MAX || ms > UINT32_MAX)
        {
            return "error usage: test_send <offer_kbps> <duration_ms>\n";
        }
        if (!start_send(static_cast<uint32_t>(kbps), static_cast<uint32_t>(ms)))
        {
            return "error busy\n";
        }
        return "ok\n";
    }
    if (cmd == "stats")
    {
        const Stats s = stats();
        char out[192];
        snprintf(out, sizeof(out),
                 "snd_pkt %llu\nsnd_byt %llu\nsnd_drop %llu\nrcv_pkt %llu\n"
                 "rcv_byt %llu\n",
                 static_cast<unsigned long long>(s.snd_pkt),
                 static_cast<unsigned long long>(s.snd_byt),
                 static_cast<unsigned long long>(s.snd_drop),
                 static_cast<unsigned long long>(s.rcv_pkt),
                 static_cast<unsigned long long>(s.rcv_byt));
        return out;
    }
    if (cmd == "clr")
    {
        clear();
        return "ok\n";
    }
    const auto it = commands_.find(cmd);
    if (it != commands_.end())
    {
        std::string args;
        std::getline(in >> std::ws, args);
        while (!args.empty() && (args.back() == '\n' || args.back() == '\r' ||
                                 args.back() == ' '))
        {
            args.pop_back();
        }
        return it->second(args);
    }
    std::string known = "test_send, stats, clr";
    for (const auto& entry : commands_)
    {
        known += ", " + entry.first;
    }
    return "error unknown command (" + known + ")\n";
}

int main(const Config& config)
{
    if (!config.upstream || config.payload_bytes == 0 ||
        config.payload_bytes > winject::k_stream_payload_max)
    {
        fprintf(stderr, "bwtest: needs an upstream and 1..%zu payload bytes\n",
                winject::k_stream_payload_max);
        return 1;
    }
    sockaddr_in addr{};
    if (!winject::parse_host_port(config.console, &addr))
    {
        fprintf(stderr, "bwtest: bad console address %s\n",
                config.console.c_str());
        return 1;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return 1;
    }
    // Wakes the loop to check stop().
    timeval timeout{};
    timeout.tv_usec = 100 * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        fprintf(stderr, "bwtest: cannot bind %s\n", config.console.c_str());
        close(fd);
        return 1;
    }

    g_stop.store(false);
    Bwtest bwtest(config.upstream, config.payload_bytes);
    for (const auto& entry : config.commands)
    {
        if (!bwtest.add_command(entry.first, entry.second))
        {
            fprintf(stderr, "bwtest: command %s ignored\n",
                    entry.first.c_str());
        }
    }
    std::thread receiver(
        [&bwtest]
        {
            while (!g_stop.load())
            {
                if (bwtest.poll_receive() == 0)
                {
                    std::this_thread::sleep_for(k_tick);
                }
            }
        });
    printf("bwtest: console %s payload %zu\n", config.console.c_str(),
           config.payload_bytes);
    char buf[256];
    while (!g_stop.load())
    {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n =
            recvfrom(fd, buf, sizeof(buf) - 1, 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0)
        {
            continue;
        }
        buf[n] = '\0';
        const std::string reply = bwtest.handle_command(buf);
        sendto(fd, reply.data(), reply.size(), 0,
               reinterpret_cast<const sockaddr*>(&from), from_len);
    }
    receiver.join();
    close(fd);
    return 0;
}

void stop()
{
    g_stop.store(true);
}

}  // namespace app_bwtest
