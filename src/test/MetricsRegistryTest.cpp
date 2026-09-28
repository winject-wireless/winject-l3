#include "utils/MetricsRegistry.h"

#include <gtest/gtest.h>
#include <variant>

using namespace winject;

TEST(MetricsRegistryTest, GetMetricsCreatesAndReturnsSameHandle)
{
    MetricsRegistry registry;
    const MetricU64 a = registry.get_metrics<MetricU64>("packets");
    const MetricU64 b = registry.get_metrics<MetricU64>("packets");
    EXPECT_EQ(a, b);
    a->store(42);
    EXPECT_EQ(b->load(), 42u);
}

TEST(MetricsRegistryTest, RebindsWhenRequestedTypeChanges)
{
    MetricsRegistry registry;
    const MetricU64 u64 = registry.get_metrics<MetricU64>("slot");
    u64->store(99);
    const MetricI64 i64 = registry.get_metrics<MetricI64>("slot");
    EXPECT_TRUE(std::holds_alternative<MetricI64>(registry.getMetrics().at("slot")));
    EXPECT_EQ(i64->load(), 0);
    i64->store(-3);
    const MetricI64 again = registry.get_metrics<MetricI64>("slot");
    EXPECT_EQ(again, i64);
    EXPECT_EQ(again->load(), -3);
}

TEST(MetricsRegistryTest, GetMetricsSnapshotAndToString)
{
    MetricsRegistry registry;
    registry.get_metrics<MetricU64>("rx")->store(7);
    registry.get_metrics<MetricF64>("rate")->store(1.5);
    const std::map<std::string, Metrics> snapshot = registry.getMetrics();
    EXPECT_EQ(snapshot.size(), 2u);
    const std::string text = to_string(snapshot);
    EXPECT_NE(text.find("rx = 7"), std::string::npos);
    EXPECT_NE(text.find("rate = 1.5"), std::string::npos);
}
