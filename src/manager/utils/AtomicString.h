#ifndef WINJECT_MANAGER_UTILS_ATOMIC_STRING_H_
#define WINJECT_MANAGER_UTILS_ATOMIC_STRING_H_

#include <mutex>
#include <string>

namespace winject
{

// std::atomic is not defined for std::string; mutex-backed load/store instead.
class AtomicString
{
public:
    AtomicString() = default;

    explicit AtomicString(const std::string& value) : value_(value) {}

    void store(const std::string& value)
    {
        std::lock_guard<std::mutex> lock(mu_);
        value_ = value;
    }

    std::string load() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return value_;
    }

private:
    mutable std::mutex mu_;
    std::string value_;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_ATOMIC_STRING_H_
