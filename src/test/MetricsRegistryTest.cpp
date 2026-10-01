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
    EXPECT_NE(text.find("rx=7"), std::string::npos);
    EXPECT_NE(text.find("rate=1.5"), std::string::npos);
}

TEST(MetricsRegistryTest, GetMetricsLookupWithoutCreate)
{
    MetricsRegistry registry;
    registry.get_metrics<MetricU64>("live")->store(11);
    const std::optional<Metrics> found = registry.get_metrics("live");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(std::get<MetricU64>(*found)->load(), 11u);
    EXPECT_FALSE(registry.get_metrics("missing").has_value());
}

TEST(MetricsRegistryTest, MetricValueToString)
{
    MetricsRegistry registry;
    registry.get_metrics<MetricI64>("neg")->store(-5);
    registry.get_metrics<MetricF64>("rate")->store(1.25);
    const std::map<std::string, Metrics> snapshot = registry.getMetrics();
    EXPECT_EQ(metric_value_to_string(snapshot.at("neg")), "-5");
    EXPECT_EQ(metric_value_to_string(snapshot.at("rate")), "1.25");
}

TEST(MetricsRegistryTest, RemovePrefix)
{
    MetricsRegistry registry;
    registry.get_metrics<MetricU64>("upstream_1_air_rx_gap_loss")->store(9);
    registry.get_metrics<MetricU64>("upstream_2_air_rx_gap_loss")->store(1);
    registry.get_metrics<MetricU64>("radio_rx_pkt")->store(3);
    registry.remove_prefix("upstream_1_");
    EXPECT_FALSE(registry.get_metrics("upstream_1_air_rx_gap_loss").has_value());
    EXPECT_TRUE(registry.get_metrics("upstream_2_air_rx_gap_loss").has_value());
    EXPECT_TRUE(registry.get_metrics("radio_rx_pkt").has_value());
}

TEST(MetricsRegistryTest, GetMetricsKeyFilter)
{
    MetricsRegistry registry;
    registry.get_metrics<MetricU64>("a")->store(1);
    registry.get_metrics<MetricU64>("b")->store(2);
    const std::map<std::string, Metrics> filtered =
        registry.getMetrics({"b", "missing", "a"});
    EXPECT_EQ(filtered.size(), 2u);
    EXPECT_EQ(std::get<MetricU64>(filtered.at("a"))->load(), 1u);
    EXPECT_EQ(std::get<MetricU64>(filtered.at("b"))->load(), 2u);
}
