#ifndef WINJECT_MANAGER_CONSOLE_SERVICE_H_
#define WINJECT_MANAGER_CONSOLE_SERVICE_H_

#include "Config.h"
#include "console/ChannelInfoView.h"
#include "utils/IOReactor.h"
#include "utils/NetUtil.h"

#include <bfc/sized_buffer.hpp>
#include <functional>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace winject
{

// Local UDP console for runtime manager controls (not the radio console).
// Binds console_in for commands; always sends replies to console_out.
// One command datagram = one line. Responses: "ok\n", "ok <args>\n", or
// "nok <msg>\n". gci args may span multiple lines in one reply datagram.
class ConsoleService
{
public:
    using set_fec_fn = std::function<bool(size_t index, FecType type, int k,
                                          int n, std::string* error)>;
    using set_budget_fn =
        std::function<bool(size_t index, size_t budget, std::string* error)>;
    using get_fec_fn = std::function<bool(size_t index, FecType* type, int* k,
                                          int* n, std::string* error)>;
    using get_budget_fn =
        std::function<bool(size_t index, size_t* budget, std::string* error)>;
    using get_ci_fn =
        std::function<bool(ChannelInfoView* out, std::string* error)>;
    using set_modulation_fn =
        std::function<bool(const std::string& name, std::string* error)>;
    using get_modulation_fn =
        std::function<bool(std::string* name, std::string* error)>;
    using set_tx_pacing_fn = std::function<bool(
        const uint32_t* max_rate_kbps, const size_t* max_data_per_tick,
        const size_t* tx_burst_size, const uint32_t* tx_burst_interval_us,
        std::string* error)>;
    using get_tx_pacing_fn =
        std::function<bool(uint32_t* max_rate_kbps, size_t* max_data_per_tick,
                           size_t* tx_burst_size,
                           uint32_t* tx_burst_interval_us, std::string* error)>;

    ~ConsoleService();

    bool start(IOReactor& reactor, const sockaddr_in& console_in,
               const sockaddr_in& console_out, set_fec_fn set_fec,
               set_budget_fn set_budget, get_fec_fn get_fec,
               get_budget_fn get_budget, get_ci_fn get_ci,
               set_modulation_fn set_modulation,
               get_modulation_fn get_modulation, set_tx_pacing_fn set_tx_pacing,
               get_tx_pacing_fn get_tx_pacing, std::string* error);
    void stop();

private:
    static constexpr size_t k_line_max = 256;

    void on_datagram();
    void reply(const char* text);
    void reply_ok();
    void reply_ok_args(const char* args);
    void reply_nok(const char* msg);
    void handle_line(const char* line);

    IOReactor* reactor = nullptr;
    bfc::socket sock;
    sockaddr_in out_addr{};
    set_fec_fn set_fec;
    set_budget_fn set_budget;
    get_fec_fn get_fec;
    get_budget_fn get_budget;
    get_ci_fn get_ci;
    set_modulation_fn set_modulation;
    get_modulation_fn get_modulation;
    set_tx_pacing_fn set_tx_pacing;
    get_tx_pacing_fn get_tx_pacing;
    bfc::sized_buffer rx_buf;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_SERVICE_H_
