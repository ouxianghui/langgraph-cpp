#include "foundation/observability/metrics.hpp"

#include "foundation/redaction/redactor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] MetricTags normalizeTags(MetricTags tags)
{
    std::ranges::sort(tags, [](const MetricTag& lhs, const MetricTag& rhs) {
        if (lhs.key_ != rhs.key_)
            return lhs.key_ < rhs.key_;
        return lhs.value_ < rhs.value_;
    });
    return tags;
}

void appendLengthPrefixed(std::string& out, std::string_view value)
{
    out.append(std::to_string(value.size()));
    out.push_back(':');
    out.append(value);
}

[[nodiscard]] MetricTags redactTags(MetricTags tags, const MetricOptions& options)
{
    if (!options.redact_ || !options.redactor_)
        return tags;

    for (auto& tag : tags) {
        if (options.redactor_->sensitiveKey(tag.key_))
            tag.value_ = options.redactor_->config().replacement_;
        else
            tag.value_ = options.redactor_->redact(tag.value_);
    }
    return tags;
}

[[nodiscard]] std::string metricKey(MetricType type, std::string_view name, const MetricTags& tags)
{
    std::string out;
    out.reserve(name.size() + tags.size() * 32U + 16U);
    out.append(std::to_string(static_cast<int>(type)));
    out.push_back('|');
    appendLengthPrefixed(out, name);
    for (const auto& tag : tags) {
        out.push_back('|');
        appendLengthPrefixed(out, tag.key_);
        out.push_back('=');
        appendLengthPrefixed(out, tag.value_);
    }
    return out;
}

[[nodiscard]] std::string toLowerAscii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char ch : value)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

[[nodiscard]] bool isHighCardinalityTagKey(std::string_view key, const MetricLimits& limits)
{
    const auto normalized = toLowerAscii(key);
    return std::ranges::any_of(limits.highCardinalityTagKeys_, [&normalized](const std::string& blocked) {
        return normalized == toLowerAscii(blocked);
    });
}

[[nodiscard]] std::size_t approxMetricBytes(std::string_view name, const MetricTags& tags, const std::vector<double>& buckets)
{
    std::size_t bytes = 192U + name.size();
    for (const auto& tag : tags)
        bytes += 64U + tag.key_.size() + tag.value_.size();
    bytes += buckets.size() * (sizeof(double) + sizeof(std::uint64_t));
    return bytes;
}

[[nodiscard]] std::vector<double> normalizeBuckets(std::vector<double> buckets)
{
    std::erase_if(buckets, [](double value) {
        return !std::isfinite(value);
    });
    std::ranges::sort(buckets);
    auto last = std::ranges::unique(buckets);
    buckets.erase(last.begin(), last.end());
    return buckets;
}

[[nodiscard]] double durationToMilliseconds(Clock::Duration duration)
{
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count())
        / 1000.0;
}

[[nodiscard]] bool validMetricNameChar(char ch) noexcept
{
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '_' || ch == '-' || ch == '.' || ch == ':' || ch == '/';
}

[[nodiscard]] Status validateMetricText(
    std::string_view value,
    std::string_view label,
    std::size_t maxLength,
    bool allowSlash)
{
    if (value.empty()) {
        std::string message(label);
        message.append(" cannot be empty");
        return Status::invalidArgument(std::move(message));
    }
    if (maxLength != 0 && value.size() > maxLength) {
        std::string message(label);
        message.append(" is too long");
        return Status::invalidArgument(std::move(message));
    }

    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) {
            std::string message(label);
            message.append(" contains a control character");
            return Status::invalidArgument(std::move(message));
        }
        if (!validMetricNameChar(ch) || (!allowSlash && ch == '/')) {
            std::string message(label);
            message.append(" contains a disallowed character");
            return Status::invalidArgument(std::move(message));
        }
    }
    return Status::ok();
}

} // namespace

InMemoryMetricRecorder::InMemoryMetricRecorder(MetricOptions options)
    : options_(std::move(options))
{
    if (options_.redact_ && !options_.redactor_)
        options_.redactor_ = std::make_shared<Redactor>();
    options_.histogramBuckets_ = normalizeBuckets(std::move(options_.histogramBuckets_));
}

Status InMemoryMetricRecorder::incrementCounter(std::string name, double delta, MetricTags tags)
{
    if (!std::isfinite(delta))
        return Status::invalidArgument("counter delta must be finite");
    if (delta <= 0.0)
        return Status::invalidArgument("counter delta must be positive");
    return record(MetricType::Counter, std::move(name), delta, std::move(tags));
}

Status InMemoryMetricRecorder::recordDuration(std::string name, Clock::Duration duration, MetricTags tags)
{
    if (duration < Clock::Duration::zero())
        return Status::invalidArgument("timer duration cannot be negative");
    return record(MetricType::Timer, std::move(name), durationToMilliseconds(duration), std::move(tags));
}

Status InMemoryMetricRecorder::recordGauge(std::string name, double value, MetricTags tags)
{
    return record(MetricType::Gauge, std::move(name), value, std::move(tags));
}

Status InMemoryMetricRecorder::recordHistogram(std::string name, double value, MetricTags tags)
{
    return record(MetricType::Histogram, std::move(name), value, std::move(tags));
}

std::vector<MetricSnapshot> InMemoryMetricRecorder::snapshots() const
{
    std::lock_guard lock(mutex_);

    std::vector<MetricSnapshot> out;
    out.reserve(metrics_.size());
    for (const auto& [_, metric] : metrics_) {
        out.push_back(MetricSnapshot {
            .type_ = metric.type_,
            .name_ = metric.name_,
            .tags_ = metric.tags_,
            .count_ = metric.count_,
            .sum_ = metric.sum_,
            .min_ = metric.count_ == 0 ? 0.0 : metric.min_,
            .max_ = metric.count_ == 0 ? 0.0 : metric.max_,
            .last_ = metric.last_,
            .bucketBounds_ = metric.bucketBounds_,
            .bucketCounts_ = metric.bucketCounts_,
        });
    }

    std::ranges::sort(out, [](const MetricSnapshot& lhs, const MetricSnapshot& rhs) {
        if (lhs.name_ != rhs.name_)
            return lhs.name_ < rhs.name_;
        return static_cast<int>(lhs.type_) < static_cast<int>(rhs.type_);
    });
    return out;
}

void InMemoryMetricRecorder::clear()
{
    std::lock_guard lock(mutex_);
    metrics_.clear();
    order_.clear();
    approxBytes_ = 0;
}

Status InMemoryMetricRecorder::flush()
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::unavailable("metric recorder is closed");
    return Status::ok();
}

Status InMemoryMetricRecorder::close()
{
    std::lock_guard lock(mutex_);
    closed_ = true;
    return Status::ok();
}

bool InMemoryMetricRecorder::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

Status InMemoryMetricRecorder::record(
    MetricType type,
    std::string name,
    double value,
    MetricTags tags)
{
    tags = redactTags(std::move(tags), options_);
    if (auto status = validateMetricName(name, options_.limits_); !status.isOk())
        return status;
    if (auto status = validateMetricTags(tags, options_.limits_); !status.isOk())
        return status;
    if (!std::isfinite(value))
        return Status::invalidArgument("metric value must be finite");

    tags = normalizeTags(std::move(tags));
    const auto key = metricKey(type, name, tags);
    const bool bucketed = type == MetricType::Timer || type == MetricType::Histogram;
    const auto metricBuckets = bucketed ? options_.histogramBuckets_ : std::vector<double> {};
    const auto metricBytes = approxMetricBytes(name, tags, metricBuckets);

    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::unavailable("metric recorder is closed");

    const auto existing = metrics_.find(key);
    if (existing == metrics_.end()
        && ((options_.limits_.maxMetrics_ != 0 && metrics_.size() >= options_.limits_.maxMetrics_)
            || (options_.limits_.maxApproxBytes_ != 0 && approxBytes_ + metricBytes > options_.limits_.maxApproxBytes_))) {
        switch (options_.overflowPolicy_) {
        case MetricOverflowPolicy::Reject:
            return Status::resourceExhausted("metric recorder is full");
        case MetricOverflowPolicy::DropNewest:
            return Status::ok();
        case MetricOverflowPolicy::DropOldest:
            while (!order_.empty()
                && ((options_.limits_.maxMetrics_ != 0 && metrics_.size() >= options_.limits_.maxMetrics_)
                    || (options_.limits_.maxApproxBytes_ != 0 && approxBytes_ + metricBytes > options_.limits_.maxApproxBytes_))) {
                const auto oldest = std::move(order_.front());
                order_.pop_front();
                const auto found = metrics_.find(oldest);
                if (found != metrics_.end()) {
                    approxBytes_ -= std::min(approxBytes_, found->second.approxBytes_);
                    metrics_.erase(found);
                }
            }
            if ((options_.limits_.maxMetrics_ != 0 && metrics_.size() >= options_.limits_.maxMetrics_)
                || (options_.limits_.maxApproxBytes_ != 0 && approxBytes_ + metricBytes > options_.limits_.maxApproxBytes_)) {
                return Status::resourceExhausted("metric recorder is full");
            }
            break;
        }
    }

    auto& metric = metrics_[key];
    if (metric.count_ == 0) {
        metric.type_ = type;
        metric.name_ = std::move(name);
        metric.tags_ = std::move(tags);
        metric.approxBytes_ = metricBytes;
        if (bucketed) {
            metric.bucketBounds_ = metricBuckets;
            metric.bucketCounts_.assign(metric.bucketBounds_.size() + 1U, 0U);
        }
        order_.push_back(key);
        approxBytes_ += metricBytes;
    }

    if (metric.count_ == std::numeric_limits<std::uint64_t>::max())
        return Status::resourceExhausted("metric count exhausted");

    const auto sum = metric.sum_ + value;
    if (!std::isfinite(sum))
        return Status::resourceExhausted("metric sum exhausted");

    ++metric.count_;
    metric.sum_ = sum;
    metric.min_ = std::min(metric.min_, value);
    metric.max_ = std::max(metric.max_, value);
    metric.last_ = value;
    if (bucketed && !metric.bucketCounts_.empty()) {
        for (std::size_t i = 0; i < metric.bucketBounds_.size(); ++i) {
            if (value <= metric.bucketBounds_[i])
                ++metric.bucketCounts_[i];
        }
        ++metric.bucketCounts_.back();
    }
    return Status::ok();
}

ScopedMetricTimer::ScopedMetricTimer(
    IMetricRecorder& recorder,
    std::string name,
    MetricTags tags,
    const Clock& clock)
    : recorder_(&recorder)
    , name_(std::move(name))
    , tags_(std::move(tags))
    , clock_(&clock)
    , startedAt_(clock.now())
    , stopped_(false)
{
}

ScopedMetricTimer::~ScopedMetricTimer()
{
    if (!stopped_)
        (void)stop();
}

ScopedMetricTimer::ScopedMetricTimer(ScopedMetricTimer&& other) noexcept
    : recorder_(other.recorder_)
    , name_(std::move(other.name_))
    , tags_(std::move(other.tags_))
    , clock_(other.clock_)
    , startedAt_(other.startedAt_)
    , stopped_(other.stopped_)
{
    other.recorder_ = nullptr;
    other.clock_ = nullptr;
    other.stopped_ = true;
}

ScopedMetricTimer& ScopedMetricTimer::operator=(ScopedMetricTimer&& other) noexcept
{
    if (this == &other)
        return *this;

    if (!stopped_)
        (void)stop();

    recorder_ = other.recorder_;
    name_ = std::move(other.name_);
    tags_ = std::move(other.tags_);
    clock_ = other.clock_;
    startedAt_ = other.startedAt_;
    stopped_ = other.stopped_;

    other.recorder_ = nullptr;
    other.clock_ = nullptr;
    other.stopped_ = true;
    return *this;
}

Status ScopedMetricTimer::stop()
{
    if (stopped_)
        return Status::ok();
    if (recorder_ == nullptr || clock_ == nullptr)
        return Status::failedPrecondition("scoped timer is not valid");

    stopped_ = true;
    return recorder_->recordDuration(name_, clock_->now() - startedAt_, tags_);
}

bool ScopedMetricTimer::stopped() const noexcept
{
    return stopped_;
}

Status validateMetricName(std::string_view name)
{
    return validateMetricName(name, MetricLimits {});
}

Status validateMetricName(std::string_view name, const MetricLimits& limits)
{
    return validateMetricText(name, "metric name", limits.maxNameLength_, true);
}

Status validateMetricTags(const MetricTags& tags, const MetricLimits& limits)
{
    if (limits.maxTags_ != 0 && tags.size() > limits.maxTags_)
        return Status::invalidArgument("metric has too many tags");

    std::vector<std::string_view> keys;
    keys.reserve(tags.size());
    for (const auto& tag : tags) {
        if (auto status = validateMetricText(tag.key_, "metric tag key", limits.maxTagKeyLength_, false); !status.isOk())
            return status;
        if (limits.maxTagValueLength_ != 0 && tag.value_.size() > limits.maxTagValueLength_)
            return Status::invalidArgument("metric tag value is too long");
        if (isHighCardinalityTagKey(tag.key_, limits))
            return Status::invalidArgument("metric tag key is reserved for high-cardinality values");
        for (const char ch : tag.value_) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte == 0x7f)
                return Status::invalidArgument("metric tag value contains a control character");
        }
        if (std::ranges::find(keys, std::string_view(tag.key_)) != keys.end())
            return Status::invalidArgument("metric tag keys must be unique");
        keys.push_back(tag.key_);
    }
    return Status::ok();
}

std::string_view metricTypeName(MetricType type) noexcept
{
    switch (type) {
    case MetricType::Counter:
        return "counter";
    case MetricType::Gauge:
        return "gauge";
    case MetricType::Timer:
        return "timer";
    case MetricType::Histogram:
        return "histogram";
    }
    return "unknown";
}

std::string_view metricOverflowPolicyName(MetricOverflowPolicy policy) noexcept
{
    switch (policy) {
    case MetricOverflowPolicy::Reject:
        return "reject";
    case MetricOverflowPolicy::DropOldest:
        return "drop_oldest";
    case MetricOverflowPolicy::DropNewest:
        return "drop_newest";
    }
    return "unknown";
}

} // namespace lgc
