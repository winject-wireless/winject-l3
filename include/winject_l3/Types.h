#ifndef WINJECT_L3_PUBLIC_TYPES_H_
#define WINJECT_L3_PUBLIC_TYPES_H_

#include <chrono>
#include <functional>
#include <memory>
#include <stdint.h>
#include <string>

namespace winject_l3
{

// Outcome of a call: ok, or an errno name as the m-plane replies it
// (docs/mplane.md § Reply conventions), or the radio's own NOK code. The API
// adds ESRCH (no node), ETIMEDOUT (no answer in time: the outcome is
// unknown), ECANCELED (the node stopped first) and EDEADLK (a blocking call
// from inside UpstreamSpec::on_queue).
struct Status
{
    std::string error;

    bool ok() const
    {
        return error.empty();
    }
};

template <class T>
struct Result
{
    Status status;
    T value{};

    bool ok() const
    {
        return status.ok();
    }
};

// Callback forms: called exactly once, on l3's callback thread (see
// winject_l3::run). Without a node they run at once on the caller's thread
// with ESRCH.
template <class T>
using Done = std::function<void(Result<T>)>;
using StatusDone = std::function<void(Status)>;

using Clock = std::chrono::steady_clock;

// Blocking calls wait this long by default. A radio call also has the radio
// m-plane's own deadline; this only bounds a wedged node.
inline constexpr std::chrono::milliseconds k_default_timeout{5000};

// vX.Y.Z; X.Y is the protocol (docs/versioning.md).
struct Version
{
    uint8_t x = 0;
    uint8_t y = 0;
    uint16_t z = 0;
};

namespace detail
{
struct NodeCore;
struct HandleAccess;
}  // namespace detail

}  // namespace winject_l3

#endif  // WINJECT_L3_PUBLIC_TYPES_H_
