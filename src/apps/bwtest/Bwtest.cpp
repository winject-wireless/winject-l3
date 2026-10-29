#include "apps/bwtest/Bwtest.h"

#include "utils/NetUtil.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
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
// Receive poll interval while the queue is empty. An idle tester costs one
// wakeup per 10 ms instead of per tick. Latency is measured at queue drain,
// so it includes this polling delay as well as receiver scheduling.
constexpr auto k_rx_idle = std::chrono::milliseconds(10);

}  // namespace

Bwtest::Bwtest(std::shared_ptr<winject::UpstreamQueue> upstream,
               size_t payload_bytes, NowUs now_us)
    : upstream_(std::move(upstream)), payload_bytes_(payload_bytes),
      now_us_(std::move(now_us))
{
    if (!now_us_)
    {
        now_us_ = [] {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        };
    }
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
    if (offer_kbps == 0 || duration_ms == 0 ||
        payload_bytes_ < k_timestamp_header_bytes ||
        payload_bytes_ > winject::k_stream_payload_max)
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
    // Versioned wire header: BWT1, sequence LE32, Unix timestamp LE64.
    std::memcpy(payload.data(), "BWT1", 4);
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
                payload[4 + i] = static_cast<uint8_t>(seq >> (8 * i));
            }
            const uint64_t timestamp = static_cast<uint64_t>(now_us_());
            for (size_t i = 0; i < 8; ++i)
            {
                payload[8 + i] = static_cast<uint8_t>(timestamp >> (8 * i));
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
        const int64_t now = now_us_();
        uint64_t timestamp = 0;
        const auto* bytes = reinterpret_cast<const uint8_t*>(packet->data());
        const bool header = packet->size() >= k_timestamp_header_bytes &&
                            std::memcmp(bytes, "BWT1", 4) == 0;
        if (header)
        {
            for (size_t i = 0; i < 8; ++i)
                timestamp |= uint64_t(bytes[8 + i]) << (8 * i);
        }
        {
            std::lock_guard<std::mutex> lock(latency_mu_);
            if (!header || !timestamp || now <= 0 || timestamp > uint64_t(now))
            {
                ++latency_.lat_invalid;
            }
            else
            {
                const uint64_t us = uint64_t(now) - timestamp;
                if (!latency_.lat_samples || us < latency_.lat_min_us)
                    latency_.lat_min_us = us;
                latency_.lat_last_us = us;
                latency_.lat_max_us = std::max(latency_.lat_max_us, us);
                ++latency_.lat_samples;
                latency_sum_us_ += us;
                latency_.lat_avg_us = latency_sum_us_ / latency_.lat_samples;
            }
        }
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
    std::lock_guard<std::mutex> lock(latency_mu_);
    Stats s = latency_;
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
    std::lock_guard<std::mutex> lock(latency_mu_);
    latency_ = {};
    latency_sum_us_ = 0;
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
        char out[512];
        snprintf(out, sizeof(out),
                 "snd_pkt %llu\nsnd_byt %llu\nsnd_drop %llu\nrcv_pkt %llu\n"
                 "rcv_byt %llu\nlat_samples %llu\nlat_invalid %llu\n"
                 "lat_last_us %llu\nlat_min_us %llu\nlat_avg_us %llu\nlat_max_us %llu\n",
                 static_cast<unsigned long long>(s.snd_pkt),
                 static_cast<unsigned long long>(s.snd_byt),
                 static_cast<unsigned long long>(s.snd_drop),
                 static_cast<unsigned long long>(s.rcv_pkt),
                 static_cast<unsigned long long>(s.rcv_byt),
                 static_cast<unsigned long long>(s.lat_samples),
                 static_cast<unsigned long long>(s.lat_invalid),
                 static_cast<unsigned long long>(s.lat_last_us),
                 static_cast<unsigned long long>(s.lat_min_us),
                 static_cast<unsigned long long>(s.lat_avg_us),
                 static_cast<unsigned long long>(s.lat_max_us));
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
    if (!config.upstream || config.payload_bytes < k_timestamp_header_bytes ||
        config.payload_bytes > winject::k_stream_payload_max)
    {
        fprintf(stderr, "bwtest: needs an upstream and 16..%zu payload bytes\n",
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
                    std::this_thread::sleep_for(k_rx_idle);
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
            // A timeout wakes the loop to check stop(); any other error
            // would repeat at once, so back off instead of spinning.
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
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
