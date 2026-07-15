#pragma once

#include "foundation/id/id_generator.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

class Redactor;

enum class SpanStatus : std::uint8_t {
    Unset,
    Ok,
    Error,
    Cancelled,
};

struct TraceContext {
    std::string traceId_;
    std::string spanId_;
    std::string parentSpanId_;
    std::string traceFlags_ { "01" };
    std::string traceState_;
    std::string baggage_;
    bool remoteParent_ { false };

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool sampled() const noexcept;
};

struct SpanEvent {
    std::string name_;
    nlohmann::json attributes_ { nlohmann::json::object() };
    Clock::TimePoint timestamp_ {};
};

struct SpanRecord {
    TraceContext context_;
    std::string name_;
    nlohmann::json attributes_ { nlohmann::json::object() };
    std::vector<SpanEvent> events_;
    Clock::TimePoint startedAt_ {};
    std::optional<Clock::TimePoint> endedAt_;
    SpanStatus status_ { SpanStatus::Unset };
    std::string statusMessage_;
};

enum class TraceOverflowPolicy : std::uint8_t {
    Reject,
    DropOldest,
    DropNewest,
};

struct TraceLimits {
    std::size_t maxSpans_ { 4096 };
    std::size_t maxNameLength_ { 128 };
    std::size_t maxAttributes_ { 32 };
    std::size_t maxAttributeKeyLength_ { 128 };
    std::size_t maxAttributeStringLength_ { 4096 };
    std::size_t maxAttributeDepth_ { 16 };
    std::size_t maxAttributeNodes_ { 4096 };
    std::size_t maxEvents_ { 64 };
    std::size_t maxStatusMessageLength_ { 1024 };
    std::size_t maxTraceStateLength_ { 512 };
    std::size_t maxBaggageLength_ { 8192 };
    std::size_t maxSpanBytes_ { 1024 * 1024 };
};

struct TraceOptions {
    TraceLimits limits_;
    TraceOverflowPolicy overflowPolicy_ { TraceOverflowPolicy::Reject };
    std::shared_ptr<IRandomSource> randomSource_;
    const Clock* clock_ { &SteadyClock::instance() };
    std::shared_ptr<const Redactor> redactor_;
    bool redact_ { true };
};

class ITraceSink {
public:
    virtual ~ITraceSink() = default;

    ITraceSink(const ITraceSink&) = delete;
    ITraceSink& operator=(const ITraceSink&) = delete;
    ITraceSink(ITraceSink&&) = delete;
    ITraceSink& operator=(ITraceSink&&) = delete;

protected:
    ITraceSink() = default;

public:
    [[nodiscard]] virtual Status recordSpan(SpanRecord span) = 0;
    [[nodiscard]] virtual Status flush() = 0;
    [[nodiscard]] virtual Status close() = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

class InMemoryTraceSink final : public ITraceSink {
public:
    explicit InMemoryTraceSink(TraceOptions options = {});
    ~InMemoryTraceSink() override = default;

    [[nodiscard]] Status recordSpan(SpanRecord span) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    [[nodiscard]] std::vector<SpanRecord> spans() const;
    [[nodiscard]] std::size_t size() const noexcept;
    void clear();

private:
    TraceOptions options_;
    mutable std::mutex mutex_;
    std::vector<SpanRecord> spans_;
    bool closed_ { false };
};

class Span final {
public:
    Span() = default;
    ~Span();

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Span(Span&& other) noexcept;
    Span& operator=(Span&& other) noexcept;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] TraceContext context() const;
    [[nodiscard]] Status lastEndStatus() const;

    [[nodiscard]] Status setAttribute(std::string key, nlohmann::json value);
    [[nodiscard]] Status addEvent(std::string name, nlohmann::json attributes = nlohmann::json::object());
    [[nodiscard]] Status setStatus(SpanStatus status, std::string message = {});
    [[nodiscard]] Status end();

private:
    friend class Tracer;

    struct State {
        explicit State(
            SpanRecord data,
            std::shared_ptr<ITraceSink> sink,
            TraceOptions options);

        mutable std::mutex mutex_;
        SpanRecord data_;
        std::shared_ptr<ITraceSink> sink_;
        TraceOptions options_;
        Status lastEndStatus_ { Status::ok() };
        bool ended_ { false };
    };

    explicit Span(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

class Tracer final {
public:
    explicit Tracer(std::shared_ptr<ITraceSink> sink, TraceOptions options = {});

    [[nodiscard]] Result<Span> startSpan(
        std::string name,
        std::optional<TraceContext> parent = std::nullopt,
        nlohmann::json attributes = nlohmann::json::object()) const;

private:
    std::shared_ptr<ITraceSink> sink_;
    TraceOptions options_;
};

[[nodiscard]] std::string_view spanStatusName(SpanStatus status) noexcept;
[[nodiscard]] std::string_view traceOverflowPolicyName(TraceOverflowPolicy policy) noexcept;
[[nodiscard]] Status validateTraceContext(const TraceContext& context);
[[nodiscard]] Status validateSpanRecord(const SpanRecord& span);
[[nodiscard]] Status validateSpanRecord(const SpanRecord& span, const TraceLimits& limits);
[[nodiscard]] Result<std::string> formatTraceParent(const TraceContext& context);
[[nodiscard]] Result<std::string> formatBaggage(const TraceContext& context);
[[nodiscard]] Result<TraceContext> parseTraceParent(
    std::string_view traceparent,
    std::string_view tracestate = {},
    std::string_view baggage = {});
[[nodiscard]] Result<TraceContext> makeRootContext();
[[nodiscard]] Result<TraceContext> makeChildContext(const TraceContext& parent);

} // namespace lgc
