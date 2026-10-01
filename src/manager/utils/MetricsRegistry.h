#ifndef WINJECT_MANAGER_UTILS_METRICS_REGISTRY_H_
#define WINJECT_MANAGER_UTILS_METRICS_REGISTRY_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#if __cplusplus >= 202002L
using atomic_uint64_t = std::atomic_uint64_t;
using atomic_int64_t = std::atomic_int64_t;
using atomic_double = std::atomic_double;
#else
using atomic_uint64_t = std::atomic<std::uint64_t>;
using atomic_int64_t = std::atomic<std::int64_t>;
using atomic_double = std::atomic<double>;
#endif

namespace winject
{

using MetricU64 = std::shared_ptr<atomic_uint64_t>;
using MetricI64 = std::shared_ptr<atomic_int64_t>;
using MetricF64 = std::shared_ptr<atomic_double>;
using Metrics = std::variant<MetricU64, MetricI64, MetricF64>;

std::string metric_value_to_string(const Metrics& metric);
std::string to_string(const std::map<std::string, Metrics>& metrics);

class MetricsRegistry
{
public:
    template <typename T>
    T get_metrics(const std::string& index);

    // Lookup only; does not create entries (use get_metrics<T> to register).
    std::optional<Metrics> get_metrics(const std::string& index) const;

    std::map<std::string, Metrics> getMetrics(
        const std::vector<std::string>& keys = {}) const;

    void remove_prefix(const std::string& prefix);

private:
    mutable std::mutex mu_;
    std::map<std::string, Metrics> metrics_;
};

namespace detail
{

template <typename T>
struct MetricTraits;

template <>
struct MetricTraits<MetricU64>
{
    static MetricU64 MakeDefault()
    {
        return std::make_shared<atomic_uint64_t>(0);
    }
};

template <>
struct MetricTraits<MetricI64>
{
    static MetricI64 MakeDefault()
    {
        return std::make_shared<atomic_int64_t>(0);
    }
};

template <>
struct MetricTraits<MetricF64>
{
    static MetricF64 MakeDefault()
    {
        return std::make_shared<atomic_double>(0.0);
    }
};

}  // namespace detail

template <typename T>
T MetricsRegistry::get_metrics(const std::string& index)
{
    static_assert(std::is_same<T, MetricU64>::value ||
                      std::is_same<T, MetricI64>::value ||
                      std::is_same<T, MetricF64>::value,
                  "get_metrics T must be MetricU64, MetricI64, or MetricF64");

    std::lock_guard<std::mutex> lock(mu_);
    const auto it = metrics_.find(index);
    if (it != metrics_.end())
    {
        if (auto* existing = std::get_if<T>(&it->second))
        {
            return *existing;
        }
        const T rebound = detail::MetricTraits<T>::MakeDefault();
        it->second = rebound;
        return rebound;
    }
    const T created = detail::MetricTraits<T>::MakeDefault();
    metrics_.emplace(index, created);
    return created;
}

}  // namespace winject

#endif  // WINJECT_MANAGER_UTILS_METRICS_REGISTRY_H_
