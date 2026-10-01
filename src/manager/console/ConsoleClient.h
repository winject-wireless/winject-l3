#ifndef WINJECT_MANAGER_CONSOLE_CLIENT_H_
#define WINJECT_MANAGER_CONSOLE_CLIENT_H_

#include "Config.h"

#include <bfc/socket.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace winject
{

struct MplaneResult
{
    bool ok = false;
    std::vector<std::string> body_lines;
    std::string payload;
    std::string error;
};

class ConsoleClient
{
public:
    using DoneFn = std::function<void(MplaneResult)>;

    ~ConsoleClient();

    bool start_connect(const Config& cfg, std::string* error);
    bool finish_connect(std::string* error);
    void close();
    int fd() const
    {
        return sock.fd();
    }
    in_addr local_ip() const
    {
        return local_ip_;
    }

    void set_default_timeout(std::chrono::milliseconds timeout)
    {
        default_timeout_ = timeout;
    }

    bool request(const std::string& mplane_line, DoneFn done);
    bool request(const std::string& mplane_line, std::chrono::milliseconds timeout,
                 DoneFn done);

    void on_line(const std::string& line);
    void poll_deadlines(std::chrono::steady_clock::time_point now);
    void cancel_pending();

    void program(const Config& cfg, DoneFn done);
    void apply_radio(const Config& cfg, uint8_t save_slot, DoneFn done);
    void query_radio_info(DoneFn done);
    void send_radio_tx(const std::string& kv_args, DoneFn done);
    void send_radio_reset(uint8_t id, DoneFn done);
    void send_ping(DoneFn done);
    void send_save_slot(uint8_t slot, DoneFn done);
    void send_load_slot(uint8_t slot, DoneFn done);
    void send_rx_filter(uint16_t domain, DoneFn done);

    void append_recv(const char* data, size_t n);
    bool pop_line(std::string* line);
    void clear_pending();

private:
    struct Pending
    {
        std::string cmd;
        std::chrono::steady_clock::time_point deadline;
        DoneFn done;
        std::vector<std::string> body_lines;
    };

    bool send_wire(const std::string& wire, std::string* error);
    void complete(uint8_t id, MplaneResult result);
    void chain_after(bool ok, const std::string& err, DoneFn next);

    bfc::socket sock;
    in_addr local_ip_{};
    std::string pending;
    uint8_t next_req_id_ = 1;
    std::map<uint8_t, Pending> pending_reqs_;
    std::chrono::milliseconds default_timeout_{3000};
};

}  // namespace winject

#endif  // WINJECT_MANAGER_CONSOLE_CLIENT_H_
