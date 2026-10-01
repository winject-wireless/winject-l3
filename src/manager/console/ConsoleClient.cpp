#include "console/ConsoleClient.h"

#include "console/MplaneCorrelation.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <vector>

namespace winject
{

namespace
{

std::string format_radio_tx_cmd(uint16_t channel, int tx_power,
                                const std::string& modulation)
{
    return "radio_tx channel=" + std::to_string(channel) + " tx_power=" +
           std::to_string(tx_power) + " modulation=" + modulation;
}

}  // namespace

ConsoleClient::~ConsoleClient()
{
    close();
}

void ConsoleClient::close()
{
    cancel_pending();
    bfc::socket(std::move(sock));
    pending.clear();
}

bool ConsoleClient::start_connect(const Config& cfg, std::string* error)
{
    close();
    in_addr ip = {};
    if (!parse_host(cfg.device, &ip))
    {
        *error = "cannot resolve " + cfg.device;
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr = ip;
    addr.sin_port = htons(cfg.console_port);
    sock = bfc::socket(bfc::create_udp4());
    if (sock.fd() < 0)
    {
        *error = strerror(errno);
        return false;
    }
    const int cr = sock.connect(addr);
    if (cr < 0 && errno != EINPROGRESS)
    {
        *error = "console " + cfg.device + ":" +
                 std::to_string(cfg.console_port) + ": " + strerror(errno);
        close();
        return false;
    }
    return true;
}

bool ConsoleClient::finish_connect(std::string* error)
{
    if (sock.fd() < 0)
    {
        *error = "no socket";
        return false;
    }
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(sock.fd(), SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0)
    {
        *error = std::string("console: ") + strerror(soerr);
        return false;
    }
    sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(sock.fd(), reinterpret_cast<sockaddr*>(&local), &len) == 0)
    {
        local_ip_ = local.sin_addr;
    }
    return true;
}

bool ConsoleClient::send_wire(const std::string& wire, std::string* error)
{
    size_t off = 0;
    while (off < wire.size())
    {
        const ssize_t n =
            send(sock.fd(), wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                *error = "console send would block";
                return false;
            }
            *error = strerror(errno);
            return false;
        }
        if (n == 0)
        {
            *error = "console closed";
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void ConsoleClient::cancel_pending()
{
    for (auto& entry : pending_reqs_)
    {
        if (entry.second.done)
        {
            MplaneResult r;
            r.ok = false;
            r.error = "cancelled";
            entry.second.done(std::move(r));
        }
    }
    pending_reqs_.clear();
}

void ConsoleClient::complete(uint8_t id, MplaneResult result)
{
    auto it = pending_reqs_.find(id);
    if (it == pending_reqs_.end())
    {
        return;
    }
    DoneFn done = std::move(it->second.done);
    pending_reqs_.erase(it);
    if (done)
    {
        done(std::move(result));
    }
}

bool ConsoleClient::request(const std::string& mplane_line, DoneFn done)
{
    return request(mplane_line, default_timeout_, std::move(done));
}

bool ConsoleClient::request(const std::string& mplane_line,
                            std::chrono::milliseconds timeout, DoneFn done)
{
    if (!done)
    {
        return false;
    }
    if (sock.fd() < 0)
    {
        MplaneResult r;
        r.error = "no socket";
        done(std::move(r));
        return false;
    }
    const uint8_t id = next_req_id_++;
    if (next_req_id_ == 0)
    {
        next_req_id_ = 1;
    }
    const std::string wire = format_mplane_cmd(id, mplane_line);
    LOG_INF(">> %s", wire.c_str());
    std::string err;
    if (!send_wire(wire, &err))
    {
        MplaneResult r;
        r.error = err;
        done(std::move(r));
        return false;
    }
    Pending p;
    p.cmd = mplane_line;
    p.deadline = std::chrono::steady_clock::now() + timeout;
    p.done = std::move(done);
    pending_reqs_[id] = std::move(p);
    return true;
}

void ConsoleClient::on_line(const std::string& line)
{
    uint8_t id = 0;
    bool ok = false;
    std::string payload;
    if (!parse_correlated_reply(line, &id, &ok, &payload))
    {
        return;
    }
    auto it = pending_reqs_.find(id);
    if (it == pending_reqs_.end())
    {
        LOG_WRN("mplane reply for unknown id %u: %s", static_cast<unsigned>(id),
                line.c_str());
        return;
    }
    LOG_INF("<< %s", line.c_str());
    if (ok && payload == "pong")
    {
        MplaneResult r;
        r.ok = true;
        r.payload = "pong";
        complete(id, std::move(r));
        return;
    }
    if (ok && !payload.empty())
    {
        it->second.body_lines.push_back(payload);
    }
    if (ok)
    {
        MplaneResult r;
        r.ok = true;
        r.body_lines = it->second.body_lines;
        r.payload = payload;
        complete(id, std::move(r));
        return;
    }
    MplaneResult r;
    r.ok = false;
    r.error = payload.empty() ? "NOK" : payload;
    complete(id, std::move(r));
}

void ConsoleClient::poll_deadlines(std::chrono::steady_clock::time_point now)
{
    for (auto it = pending_reqs_.begin(); it != pending_reqs_.end();)
    {
        if (now < it->second.deadline)
        {
            ++it;
            continue;
        }
        DoneFn done = std::move(it->second.done);
        pending_reqs_.erase(it++);
        if (done)
        {
            MplaneResult r;
            r.ok = false;
            r.error = "timeout";
            done(std::move(r));
        }
    }
}

void ConsoleClient::append_recv(const char* data, size_t n)
{
    if (data == nullptr || n == 0)
    {
        return;
    }
    pending.append(data, n);
}

bool ConsoleClient::pop_line(std::string* line)
{
    const auto nl = pending.find('\n');
    if (nl == std::string::npos)
    {
        return false;
    }
    *line = pending.substr(0, nl);
    if (!line->empty() && line->back() == '\r')
    {
        line->pop_back();
    }
    pending.erase(0, nl + 1);
    return true;
}

void ConsoleClient::clear_pending()
{
    pending.clear();
}

void ConsoleClient::send_ping(DoneFn done)
{
    request("ping", std::move(done));
}

void ConsoleClient::send_rx_filter(uint16_t domain, DoneFn done)
{
    if (domain == 0)
    {
        MplaneResult r;
        r.ok = true;
        if (done)
        {
            done(std::move(r));
        }
        return;
    }
    std::string mac;
    if (!domain_to_filter_mac(domain, &mac))
    {
        MplaneResult r;
        r.error = "invalid domain for rx filter";
        if (done)
        {
            done(std::move(r));
        }
        return;
    }
    request("rx_filter_addr3 addr=" + mac, std::move(done));
}

void ConsoleClient::send_save_slot(uint8_t slot, DoneFn done)
{
    request("save " + std::to_string(slot), std::move(done));
}

void ConsoleClient::send_load_slot(uint8_t slot, DoneFn done)
{
    request("load " + std::to_string(slot), std::move(done));
}

void ConsoleClient::query_radio_info(DoneFn done)
{
    request("radio_tx_info",
            [done = std::move(done)](MplaneResult r)
            {
                if (!r.ok)
                {
                    done(std::move(r));
                    return;
                }
                if (r.body_lines.empty() && !r.payload.empty())
                {
                    r.body_lines.push_back(r.payload);
                }
                done(std::move(r));
            });
}

void ConsoleClient::send_radio_tx(const std::string& kv_args, DoneFn done)
{
    if (kv_args.empty())
    {
        MplaneResult r;
        r.error = "empty radio_tx";
        if (done)
        {
            done(std::move(r));
        }
        return;
    }
    request("radio_tx " + kv_args, std::move(done));
}

void ConsoleClient::send_radio_reset(uint8_t id, DoneFn done)
{
    request("reset id=" + std::to_string(id), std::move(done));
}

void ConsoleClient::apply_radio(const Config& cfg, uint8_t save_slot,
                                DoneFn done)
{
    request(format_radio_tx_cmd(cfg.channel, cfg.power_dbm, cfg.modulation),
            [this, cfg, save_slot, done = std::move(done)](MplaneResult r1)
            {
                if (!r1.ok)
                {
                    done(std::move(r1));
                    return;
                }
                send_rx_filter(
                    cfg.domain,
                    [this, cfg, save_slot,
                     done = std::move(done)](MplaneResult r2)
                    {
                        if (!r2.ok)
                        {
                            done(std::move(r2));
                            return;
                        }
                        send_save_slot(
                            save_slot,
                            [cfg, save_slot, done = std::move(done)](MplaneResult r3)
                            {
                                if (!r3.ok)
                                {
                                    done(std::move(r3));
                                    return;
                                }
                                LOG_INF(
                                    "radio programmed ch=%u mod=%s pwr=%d "
                                    "domain=%s save=%u",
                                    cfg.channel, cfg.modulation.c_str(),
                                    cfg.power_dbm,
                                    domain_to_string(cfg.domain).c_str(),
                                    static_cast<unsigned>(save_slot));
                                MplaneResult ok;
                                ok.ok = true;
                                done(std::move(ok));
                            });
                    });
            });
}

void ConsoleClient::program(const Config& cfg, DoneFn done)
{
    std::string err;
    if (!start_connect(cfg, &err) || !finish_connect(&err))
    {
        MplaneResult r;
        r.error = err;
        if (done)
        {
            done(std::move(r));
        }
        return;
    }
    apply_radio(cfg, cfg.config_slot,
                [this, done = std::move(done)](MplaneResult r)
                {
                    if (!r.ok)
                    {
                        close();
                        done(std::move(r));
                        return;
                    }
                    pending.clear();
                    done(std::move(r));
                });
}

}  // namespace winject
