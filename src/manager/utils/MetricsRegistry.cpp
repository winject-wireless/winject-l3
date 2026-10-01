#include "utils/MetricsRegistry.h"

#include <cstdio>
#include <sstream>

namespace winject
{

std::string metric_value_to_string(const Metrics& metric)
{
    return std::visit(
        [](const auto& handle) -> std::string
        {
            using Handle = std::decay_t<decltype(handle)>;
            if constexpr (std::is_same_v<Handle, MetricF64>)
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", handle->load());
                return buf;
            }
            else
            {
                return std::to_string(handle->load());
            }
        },
        metric);
}

std::string to_string(const std::map<std::string, Metrics>& metrics)
{
    std::ostringstream out;
    for (const auto& entry : metrics)
    {
        out << entry.first << '=';
        out << metric_value_to_string(entry.second);
        out << '\n';
    }
    return out.str();
}

std::optional<Metrics> MetricsRegistry::get_metrics(
    const std::string& index) const
{
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = metrics_.find(index);
    if (it == metrics_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::map<std::string, Metrics> MetricsRegistry::getMetrics(
    const std::vector<std::string>& keys) const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (keys.empty())
    {
        return metrics_;
    }
    std::map<std::string, Metrics> filtered;
    for (const std::string& key : keys)
    {
        const auto it = metrics_.find(key);
        if (it != metrics_.end())
        {
            filtered.emplace(it->first, it->second);
        }
    }
    return filtered;
}

void MetricsRegistry::remove_prefix(const std::string& prefix)
{
    if (prefix.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = metrics_.begin(); it != metrics_.end();)
    {
        if (it->first.rfind(prefix, 0) == 0)
        {
            it = metrics_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

}  // namespace winject
