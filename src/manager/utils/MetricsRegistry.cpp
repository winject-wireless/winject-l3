#include "utils/MetricsRegistry.h"

#include <sstream>

namespace winject
{

std::string to_string(const std::map<std::string, Metrics>& metrics)
{
    std::ostringstream out;
    for (const auto& entry : metrics)
    {
        out << entry.first << " = ";
        std::visit(
            [&](const auto& metric)
            {
                out << metric->load();
            },
            entry.second);
        out << '\n';
    }
    return out.str();
}

std::map<std::string, Metrics> MetricsRegistry::getMetrics()
{
    std::lock_guard<std::mutex> lock(mu_);
    return metrics_;
}

}  // namespace winject
