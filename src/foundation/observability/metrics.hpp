#pragma once

#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lc {

class Redactor;

struct MetricTag {
    std::string key_;
    std::string value_;

    friend bool operator==(const MetricTag&, const MetricTag&) = default;
};

using MetricTags = std::vector<MetricTag>;

enum class MetricType : std::uint8_t {
    Counter,
    Gauge,
    Timer,
    Histogram,
};

enum class MetricOverflowPolicy : std::uint8_t {
    Reject,
    DropOldest,
    DropNewest,
};

struct MetricLimits {
    std::size_t maxMetrics_ { 4096 };
    std::size_t maxNameLength_ { 128 };
    std::size_t maxTags_ { 16 };
    std::size_t maxTagKeyLength_ { 64 };
    std::size_t maxTagValueLength_ { 512 };
    std::size_t maxApproxBytes_ { 2 * 1024 * 1024 };
    std::vector<std::string> highCardinalityTagKeys_ {
        "run_id",
        "thread_id",
        "user_id",
        "session_id",
        "request_id",
        "trace_id",
        "span_id",
        "checkpoint_id",
        "node_execution_id",
    };
};

struct MetricOptions {
    MetricLimits limits_;
    MetricOverflowPolicy overflowPolicy_ { MetricOverflowPolicy::Reject };
    std::shared_ptr<const Redactor> redactor_;
    std::vector<double> histogramBuckets_ { 0.0, 1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0, 10000.0 };
    bool redact_ { true };
};

struct MetricSnapshot {
    MetricType type_ { MetricType::Counter };
    std::string name_;
    MetricTags tags_;
    std::uint64_t count_ { 0 };
    double sum_ { 0.0 };
    double min_ { 0.0 };
    double max_ { 0.0 };
    double last_ { 0.0 };
    std::vector<double> bucketBounds_;
    std::vector<std::uint64_t> bucketCounts_;
};

class IMetricRecorder {
public:
    virtual ~IMetricRecorder() = default;

    IMetricRecorder(const IMetricRecorder&) = delete;
    IMetricRecorder& operator=(const IMetricRecorder&) = delete;
    IMetricRecorder(IMetricRecorder&&) = delete;
    IMetricRecorder& operator=(IMetricRecorder&&) = delete;

protected:
    IMetricRecorder() = default;

public:
    [[nodiscard]] virtual Status incrementCounter(
        std::string name,
        double delta = 1.0,
        MetricTags tags = {})
        = 0;

    [[nodiscard]] virtual Status recordDuration(
        std::string name,
        Clock::Duration duration,
        MetricTags tags = {})
        = 0;

    [[nodiscard]] virtual Status recordGauge(
        std::string name,
        double value,
        MetricTags tags = {})
        = 0;

    [[nodiscard]] virtual Status recordHistogram(
        std::string name,
        double value,
        MetricTags tags = {})
        = 0;

    [[nodiscard]] virtual std::vector<MetricSnapshot> snapshots() const = 0;
    [[nodiscard]] virtual Status flush() = 0;
    [[nodiscard]] virtual Status close() = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
    virtual void clear() = 0;
};

class InMemoryMetricRecorder final : public IMetricRecorder {
public:
    explicit InMemoryMetricRecorder(MetricOptions options = {});
    ~InMemoryMetricRecorder() override = default;

    [[nodiscard]] Status incrementCounter(
        std::string name,
        double delta = 1.0,
        MetricTags tags = {}) override;

    [[nodiscard]] Status recordDuration(
        std::string name,
        Clock::Duration duration,
        MetricTags tags = {}) override;

    [[nodiscard]] Status recordGauge(
        std::string name,
        double value,
        MetricTags tags = {}) override;

    [[nodiscard]] Status recordHistogram(
        std::string name,
        double value,
        MetricTags tags = {}) override;

    [[nodiscard]] std::vector<MetricSnapshot> snapshots() const override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    void clear() override;

private:
    struct Accumulator {
        MetricType type_ { MetricType::Counter };
        std::string name_;
        MetricTags tags_;
        std::uint64_t count_ { 0 };
        double sum_ { 0.0 };
        double min_ { std::numeric_limits<double>::infinity() };
        double max_ { -std::numeric_limits<double>::infinity() };
        double last_ { 0.0 };
        std::size_t approxBytes_ { 0 };
        std::vector<double> bucketBounds_;
        std::vector<std::uint64_t> bucketCounts_;
    };

    [[nodiscard]] Status record(
        MetricType type,
        std::string name,
        double value,
        MetricTags tags);

    MetricOptions options_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Accumulator> metrics_;
    std::deque<std::string> order_;
    std::size_t approxBytes_ { 0 };
    bool closed_ { false };
};

class ScopedMetricTimer final {
public:
    ScopedMetricTimer(
        IMetricRecorder& recorder,
        std::string name,
        MetricTags tags = {},
        const Clock& clock = SteadyClock::instance());
    ~ScopedMetricTimer();

    ScopedMetricTimer(const ScopedMetricTimer&) = delete;
    ScopedMetricTimer& operator=(const ScopedMetricTimer&) = delete;
    ScopedMetricTimer(ScopedMetricTimer&& other) noexcept;
    ScopedMetricTimer& operator=(ScopedMetricTimer&& other) noexcept;

    [[nodiscard]] Status stop();
    [[nodiscard]] bool stopped() const noexcept;

private:
    IMetricRecorder* recorder_ { nullptr };
    std::string name_;
    MetricTags tags_;
    const Clock* clock_ { nullptr };
    Clock::TimePoint startedAt_ {};
    bool stopped_ { true };
};

[[nodiscard]] Status validateMetricName(std::string_view name);
[[nodiscard]] Status validateMetricName(std::string_view name, const MetricLimits& limits);
[[nodiscard]] Status validateMetricTags(const MetricTags& tags, const MetricLimits& limits = {});
[[nodiscard]] std::string_view metricTypeName(MetricType type) noexcept;
[[nodiscard]] std::string_view metricOverflowPolicyName(MetricOverflowPolicy policy) noexcept;

} // namespace lc
