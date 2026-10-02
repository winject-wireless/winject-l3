#include "Config.h"

#include "fec/RsBlockErasure.h"
#include "radio/PhyAirtime.h"
#include "utils/Log.h"
#include "utils/NetUtil.h"

#include <bfc/configuration_parser.hpp>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace winject
{

static std::string trim(const std::string& s)
{
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool parse_mode(const std::string& text, UpstreamMode* mode)
{
    if (text == "UDP_STATIC_FORWARDING" || text == "UDP_GENERIC_FORWARDING")
    {
        *mode = UpstreamMode::udp_static;
        return true;
    }
    if (text == "UDP_CLIENT_FORWARDING")
    {
        *mode = UpstreamMode::udp_client;
        return true;
    }
    if (text == "UDP_SERVER_FORWARDING")
    {
        *mode = UpstreamMode::udp_server;
        return true;
    }
    return false;
}

static std::string key_of(size_t i, const char* field)
{
    std::ostringstream os;
    os << "upstream-" << i << "." << field;
    return os.str();
}

static bool require_arg(const bfc::configuration_parser& p,
                        const std::string& key, std::string* out,
                        std::string* error)
{
    auto v = p.arg(key);
    if (!v || v->empty())
    {
        *error = "missing " + key;
        return false;
    }
    *out = *v;
    return true;
}

uint32_t Config::phy_rate_kbps(const std::string& modulation)
{
    return phy_nominal_kbps(modulation);
}

std::string Config::canonical_modulation(const std::string& modulation)
{
    return phy_canonical_name(modulation);
}

bool Config::modulation_ok_for_channel(const std::string& modulation,
                                       uint8_t channel)
{
    const std::string name = canonical_modulation(modulation);
    if (name.empty())
    {
        return false;
    }
    if (channel != 14)
    {
        return true;
    }
    return name.rfind("DSS_", 0) == 0 || name.rfind("CCK_", 0) == 0;
}

bool Config::load(const std::string& path, std::string* error)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        *error = "cannot open " + path;
        return false;
    }

    bfc::configuration_parser parser;
    std::string line;
    while (std::getline(in, line))
    {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#')
        {
            continue;
        }
        parser.load_line(t);
    }

    if (!require_arg(parser, "winject.device", &device, error))
    {
        return false;
    }
    auto console = parser.as<unsigned>("winject.console");
    if (!console || *console == 0 || *console > 65535)
    {
        *error = "invalid winject.console";
        return false;
    }
    console_port = static_cast<uint16_t>(*console);

    auto ch = parser.as<unsigned>("winject.channel");
    if (!ch || *ch < 1 || *ch > 14)
    {
        *error = "invalid winject.channel";
        return false;
    }
    channel = static_cast<uint8_t>(*ch);

    if (!require_arg(parser, "winject.modulation", &modulation, error))
    {
        return false;
    }
    const std::string canon = canonical_modulation(modulation);
    if (canon.empty())
    {
        *error = "invalid winject.modulation";
        return false;
    }
    modulation = canon;
    if (!modulation_ok_for_channel(modulation, channel))
    {
        *error = "winject.modulation not valid for winject.channel";
        return false;
    }
    auto pwr = parser.as<int>("winject.power");
    if (!pwr || *pwr < 2 || *pwr > 20)
    {
        *error = "invalid winject.power";
        return false;
    }
    power_dbm = static_cast<int8_t>(*pwr);

    std::string domain_s;
    if (!require_arg(parser, "winject.domain", &domain_s, error))
    {
        return false;
    }
    if (!parse_domain(domain_s, &domain))
    {
        *error = "invalid winject.domain";
        return false;
    }
    if (domain == 0)
    {
        *error = "winject.domain must be non-zero";
        return false;
    }

    auto rate = parser.as<unsigned>("winject.max_rate_kbps");
    if (rate && *rate > 0)
    {
        max_rate_kbps = *rate;
        max_rate_kbps_explicit = true;
    }
    else
    {
        max_rate_kbps = 0;
        max_rate_kbps_explicit = false;
    }
    if (auto gap = parser.as<unsigned>("winject.tx_gap_us"))
    {
        if (*gap > 10000u)
        {
            *error = "invalid winject.tx_gap_us";
            return false;
        }
        tx_gap_us = static_cast<uint32_t>(*gap);
    }
    local_ip = parser.arg("winject.local_ip").value_or("");
    if (auto skip = parser.arg("winject.skip_console"))
    {
        skip_console = *skip == "1" || *skip == "true";
    }
    if (auto c = parser.arg("winject.cca"))
    {
        if (*c == "1" || *c == "true")
        {
            cca = true;
        }
        else if (*c == "0" || *c == "false")
        {
            cca = false;
        }
        else
        {
            *error = "invalid winject.cca";
            return false;
        }
        cca_explicit = true;
    }
    if (auto fcs = parser.arg("winject.radio_fcs"))
    {
        std::string v;
        for (char c : *fcs)
        {
            v.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (v == "auto")
        {
            radio_fcs = RadioFcsConfig::auto_detect;
        }
        else if (v == "signal")
        {
            radio_fcs = RadioFcsConfig::signal;
        }
        else if (v == "actual")
        {
            radio_fcs = RadioFcsConfig::actual;
        }
        else
        {
            *error = "invalid winject.radio_fcs";
            return false;
        }
    }
    if (auto mpt = parser.as<unsigned>("winject.max_data_per_tick"))
    {
        if (*mpt < 1 || *mpt > 32)
        {
            *error = "invalid winject.max_data_per_tick";
            return false;
        }
        max_data_per_tick = *mpt;
    }
    if (auto bs = parser.as<unsigned>("winject.tx_burst_size"))
    {
        if (*bs < 1 || *bs > k_radio_tx_queue_depth)
        {
            *error = "invalid winject.tx_burst_size";
            return false;
        }
        tx_burst_size = *bs;
    }
    if (auto bi = parser.as<unsigned>("winject.tx_burst_interval_us"))
    {
        if (*bi > 1000000u)
        {
            *error = "invalid winject.tx_burst_interval_us";
            return false;
        }
        tx_burst_interval_us = static_cast<uint32_t>(*bi);
    }
    manager_console_in = parser.arg("manager.console_in").value_or("");
    manager_console_out = parser.arg("manager.console_out").value_or("");
    if (manager_console_in.empty() != manager_console_out.empty())
    {
        *error = "manager.console_in and manager.console_out must both be set";
        return false;
    }
    if (!manager_console_in.empty())
    {
        sockaddr_in tmp = {};
        if (!parse_host_port(manager_console_in, &tmp))
        {
            *error = "invalid manager.console_in";
            return false;
        }
        if (!parse_host_port(manager_console_out, &tmp))
        {
            *error = "invalid manager.console_out";
            return false;
        }
    }
    auto fwd_port = parser.as<unsigned>("winject.forward_port");
    auto fwd_base = parser.as<unsigned>("winject.forward_base");
    if (fwd_port && *fwd_port > 0 && *fwd_port <= 65535)
    {
        forward_port = static_cast<uint16_t>(*fwd_port);
        forward_base = forward_port;
    }
    else if (fwd_base && *fwd_base > 0 && *fwd_base <= 65535)
    {
        forward_base = static_cast<uint16_t>(*fwd_base);
        forward_port = forward_base;
    }
    auto inj = parser.as<unsigned>("winject.inject_port");
    if (inj && *inj > 0 && *inj <= 65535)
    {
        inject_port = static_cast<uint16_t>(*inj);
    }
    stats_sec = parser.as<unsigned>("winject.stats_sec").value_or(0);
    if (const char* env = std::getenv("WINJECT_STATS_SEC"))
    {
        const unsigned v =
            static_cast<unsigned>(std::strtoul(env, nullptr, 10));
        if (v > 0)
        {
            stats_sec = v;
        }
    }

    auto size = parser.as<unsigned>("upstream.size");
    if (!size || *size == 0)
    {
        *error = "invalid upstream.size";
        return false;
    }
    upstreams.clear();
    for (size_t i = 0; i < *size; i++)
    {
        UpstreamConfig u;
        u.index = i;
        std::string umode;
        if (!require_arg(parser, key_of(i, "mode"), &umode, error))
        {
            return false;
        }
        UpstreamMode mode = UpstreamMode::udp_static;
        if (!parse_mode(umode, &mode))
        {
            *error = "invalid " + key_of(i, "mode");
            return false;
        }
        // Firmware TX/RX are independent and optional.
        // Preferred: upstream-N.tx_bus / rx_bus.
        // Legacy: upstream_tx-N.bus / upstream_rx-N.bus, or bus_tx / bus_rx.
        const std::string bus_tx_s =
            parser.arg(key_of(i, "tx_bus"))
                .value_or(
                    parser.arg("upstream_tx-" + std::to_string(i) + ".bus")
                        .value_or(
                            parser.arg(key_of(i, "bus_tx")).value_or("")));
        const std::string bus_rx_s =
            parser.arg(key_of(i, "rx_bus"))
                .value_or(
                    parser.arg("upstream_rx-" + std::to_string(i) + ".bus")
                        .value_or(
                            parser.arg(key_of(i, "bus_rx")).value_or("")));
        if (!bus_tx_s.empty())
        {
            if (!parse_bus(bus_tx_s, &u.bus_tx) || u.bus_tx == 0)
            {
                *error = "invalid " + key_of(i, "tx_bus");
                return false;
            }
        }
        if (!bus_rx_s.empty())
        {
            if (!parse_bus(bus_rx_s, &u.bus_rx) || u.bus_rx == 0)
            {
                *error = "invalid " + key_of(i, "rx_bus");
                return false;
            }
        }
        if (u.bus_tx == 0 && u.bus_rx == 0)
        {
            *error =
                "upstream-" + std::to_string(i) + " needs tx_bus and/or rx_bus";
            return false;
        }
        auto budget = parser.as<unsigned>(key_of(i, "scheduler_budget"));
        if (!budget || *budget == 0)
        {
            *error = "invalid " + key_of(i, "scheduler_budget");
            return false;
        }
        u.scheduler_budget = *budget;
        std::string rx = parser.arg(key_of(i, "rx")).value_or("");
        std::string tx = parser.arg(key_of(i, "tx")).value_or("");
        const std::string bind_address =
            parser.arg(key_of(i, "bind_address")).value_or("");
        const std::string connect_address =
            parser.arg(key_of(i, "connect_address")).value_or("");
        if (!bind_address.empty())
        {
            if (!rx.empty() && rx != bind_address)
            {
                *error = key_of(i, "rx") + " and " + key_of(i, "bind_address") +
                         " conflict";
                return false;
            }
            if (rx.empty())
            {
                rx = bind_address;
            }
        }
        if (!connect_address.empty())
        {
            if (!tx.empty() && tx != connect_address)
            {
                *error = key_of(i, "tx") + " and " +
                         key_of(i, "connect_address") + " conflict";
                return false;
            }
            if (tx.empty())
            {
                tx = connect_address;
            }
        }

        const std::string fec_type =
            parser.arg(key_of(i, "fec.type")).value_or("");
        if (!fec_type.empty() && fec_type != "NONE")
        {
            if (fec_type != "RS_BLOCK_ERASURE")
            {
                *error = "invalid " + key_of(i, "fec.type");
                return false;
            }
            auto fk = parser.as<unsigned>(key_of(i, "fec.k"));
            auto fn = parser.as<unsigned>(key_of(i, "fec.n"));
            if (!fk || !fn || *fk < 1 || *fn <= *fk || *fn > 255)
            {
                *error = "invalid " + key_of(i, "fec.k") + "/" +
                         key_of(i, "fec.n") + " (need 1 <= k < n <= 255)";
                return false;
            }
            u.fec_type = FecType::RsBlockErasure;
            u.fec_k = static_cast<int>(*fk);
            u.fec_n = static_cast<int>(*fn);
            auto fto = parser.as<int>(key_of(i, "fec.timeout_ms"));
            if (fto)
            {
                if (*fto < 0)
                {
                    *error = "invalid " + key_of(i, "fec.timeout_ms");
                    return false;
                }
                u.fec_timeout_ms = *fto;
            }
        }

        if (rx.empty() && tx.empty())
        {
            *error = "upstream-" + std::to_string(i) +
                     " needs rx and tx (static), rx only (server), or tx only "
                     "(client)";
            return false;
        }
        const UpstreamMode inferred = upstream_mode(UdpPeerEndpoint{rx, tx});
        if (mode != inferred)
        {
            *error = key_of(i, "mode") + " does not match rx/tx";
            return false;
        }
        u.endpoint = UdpPeerEndpoint{std::move(rx), std::move(tx)};
        upstreams.push_back(u);
    }
    if (!validate_upstreams(upstreams, error))
    {
        return false;
    }
    if (skip_console && radio_fcs == RadioFcsConfig::auto_detect)
    {
        *error =
            "winject.skip_console requires winject.radio_fcs=signal|actual";
        return false;
    }
    return true;
}

bool Config::validate_upstreams(const std::vector<UpstreamConfig>& upstreams,
                                std::string* error)
{
    for (size_t i = 0; i < upstreams.size(); ++i)
    {
        const UpstreamConfig& u = upstreams[i];
        if (u.bus_tx == 0 && u.bus_rx == 0)
        {
            if (error != nullptr)
            {
                *error = "upstream needs tx_bus and/or rx_bus";
            }
            return false;
        }
    }
    for (uint8_t bus = 1; bus != 0; ++bus)
    {
        size_t count = 0;
        bool fec_on_bus = false;
        for (const auto& u : upstreams)
        {
            if (u.bus_tx != bus)
            {
                continue;
            }
            ++count;
            if (u.fec_type == FecType::RsBlockErasure)
            {
                fec_on_bus = true;
            }
        }
        if (count > 1 && fec_on_bus)
        {
            if (error != nullptr)
            {
                *error = "shared tx_bus with FEC enabled";
            }
            return false;
        }
    }
    return true;
}

bool Config::validate_upstream_update(const UpstreamConfig& current,
                                      FecType fec_type, int fec_k, int fec_n,
                                      int fec_timeout_ms, size_t quanta,
                                      bool have_fec, bool have_k, bool have_n,
                                      bool have_fec_timeout, bool have_quanta,
                                      const std::vector<UpstreamConfig>& all,
                                      UpstreamConfig* out, std::string* error)
{
    if (out == nullptr)
    {
        if (error != nullptr)
        {
            *error = "invalid argument";
        }
        return false;
    }
    *out = current;
    if (have_quanta)
    {
        if (quanta == 0)
        {
            if (error != nullptr)
            {
                *error = "invalid quanta";
            }
            return false;
        }
        out->scheduler_budget = quanta;
    }
    if (have_fec_timeout)
    {
        if (fec_timeout_ms < 1)
        {
            if (error != nullptr)
            {
                *error = "invalid fec_timeout";
            }
            return false;
        }
        out->fec_timeout_ms = fec_timeout_ms;
    }
    FecType type = current.fec_type;
    int k = current.fec_k;
    int n = current.fec_n;
    if (have_fec)
    {
        type = fec_type;
    }
    if (have_k)
    {
        k = fec_k;
    }
    if (have_n)
    {
        n = fec_n;
    }
    if (type == FecType::none)
    {
        k = 0;
        n = 0;
    }
    else
    {
        RsBlockErasure codec;
        const int timeout =
            have_fec_timeout ? fec_timeout_ms : out->fec_timeout_ms;
        if (!codec.init(k, n, timeout))
        {
            if (error != nullptr)
            {
                *error = "invalid fec k/n";
            }
            return false;
        }
    }
    out->fec_type = type;
    out->fec_k = k;
    out->fec_n = n;
    std::vector<UpstreamConfig> candidate = all;
    for (auto& u : candidate)
    {
        if (u.index == current.index)
        {
            u = *out;
            break;
        }
    }
    if (!validate_upstreams(candidate, error))
    {
        return false;
    }
    return true;
}

}  // namespace winject
