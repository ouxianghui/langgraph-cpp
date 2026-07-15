#pragma once

#include "foundation/event/i_event_sink.hpp"
#include "foundation/event/runtime_event.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/observability/tracing.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

struct RedactionConfig {
    std::string replacement_ { "[REDACTED]" };
    std::vector<std::string> sensitiveKeys_;
    std::vector<std::string> sensitiveKeySubstrings_;
    bool redactSensitiveKeys_ { true };
    bool redactStringValues_ { true };
    bool redactEmailAddresses_ { true };
    bool redactCreditCardNumbers_ { true };
    std::size_t maxDepth_ { 32 };
    std::size_t maxStringLengthToScan_ { 4096 };
    std::size_t maxJsonNodes_ { 4096 };
    std::size_t maxObjectSize_ { 1024 };
    std::size_t maxArraySize_ { 4096 };
    std::size_t maxOutputBytes_ { 1024U * 1024U };

    [[nodiscard]] static RedactionConfig defaults();
};

struct RedactionReport {
    std::size_t fields_ { 0 };
    std::size_t strings_ { 0 };

    [[nodiscard]] bool changed() const noexcept { return fields_ != 0 || strings_ != 0; }
};

template <typename T>
struct RedactionResult final {
    T value_;
    RedactionReport report_;

    [[nodiscard]] bool changed() const noexcept { return report_.changed(); }
};

class Redactor final {
public:
    explicit Redactor(RedactionConfig config = RedactionConfig::defaults());

    [[nodiscard]] const RedactionConfig& config() const noexcept;
    [[nodiscard]] bool sensitiveKey(std::string_view key) const;

    [[nodiscard]] RedactionResult<std::string> redactWithReport(std::string_view value) const;
    [[nodiscard]] RedactionResult<std::string> redactWithReport(const std::string& value) const;
    [[nodiscard]] std::string redact(std::string_view value) const;
    [[nodiscard]] std::string redact(const std::string& value) const;
    [[nodiscard]] std::string redact(const char* value) const;

    [[nodiscard]] RedactionResult<nlohmann::json> redactWithReport(const nlohmann::json& value) const;
    [[nodiscard]] nlohmann::json redact(const nlohmann::json& value) const;

    [[nodiscard]] RuntimeEvent redact(RuntimeEvent event) const;
    [[nodiscard]] SpanEvent redact(SpanEvent event) const;
    [[nodiscard]] SpanRecord redact(SpanRecord span) const;
    [[nodiscard]] Result<State> redact(const State& state) const;
    [[nodiscard]] Result<CheckpointTask> redact(const CheckpointTask& task) const;
    [[nodiscard]] Result<CheckpointWrite> redact(const CheckpointWrite& write) const;
    [[nodiscard]] Result<Checkpoint> redact(const Checkpoint& checkpoint) const;

private:
    [[nodiscard]] RedactionResult<nlohmann::json> redactJsonValue(
        const nlohmann::json& value,
        std::size_t depth) const;
    [[nodiscard]] RedactionResult<nlohmann::json> redactJsonValue(
        const nlohmann::json& value,
        std::size_t depth,
        std::size_t& nodes,
        std::size_t& outputBytes) const;

    RedactionConfig config_;
    std::vector<std::string> normalizedSensitiveKeys_;
    std::vector<std::string> normalizedSensitiveKeySubstrings_;
};

class RedactionLogger final : public ILogger {
public:
    RedactionLogger(std::shared_ptr<ILogger> inner, Redactor redactor = Redactor());

    void log(const LogRecord& record) noexcept override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    std::shared_ptr<ILogger> inner_;
    Redactor redactor_;
};

class RedactionEventSink final : public IEventSink {
public:
    RedactionEventSink(std::shared_ptr<IEventSink> inner, Redactor redactor = Redactor());
    ~RedactionEventSink() override = default;

    [[nodiscard]] Status publish(RuntimeEvent event) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status waitIdle(Duration timeout) override;
    [[nodiscard]] Status close(Duration waitIdleTimeout) override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    std::shared_ptr<IEventSink> inner_;
    Redactor redactor_;
};

class RedactionTraceSink final : public ITraceSink {
public:
    RedactionTraceSink(std::shared_ptr<ITraceSink> inner, Redactor redactor = Redactor());
    ~RedactionTraceSink() override = default;

    [[nodiscard]] Status recordSpan(SpanRecord span) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    std::shared_ptr<ITraceSink> inner_;
    Redactor redactor_;
};

class RedactionStateCodec final : public IStateCodec {
public:
    RedactionStateCodec(std::shared_ptr<IStateCodec> inner, Redactor redactor = Redactor());

    [[nodiscard]] Result<Payload> encode(const State& state) const override;
    [[nodiscard]] Result<State> decode(const Payload& payload) const override;

private:
    std::shared_ptr<IStateCodec> inner_;
    Redactor redactor_;
};

class RedactionCheckpointCodec final : public ICheckpointCodec {
public:
    RedactionCheckpointCodec(std::shared_ptr<ICheckpointCodec> inner, Redactor redactor = Redactor());

    [[nodiscard]] Result<Payload> encode(const Checkpoint& checkpoint) const override;
    [[nodiscard]] Result<Checkpoint> decode(const Payload& payload) const override;
    [[nodiscard]] Result<Payload> encodeWrite(const CheckpointWrite& write) const override;
    [[nodiscard]] Result<CheckpointWrite> decodeWrite(const Payload& payload) const override;

private:
    std::shared_ptr<ICheckpointCodec> inner_;
    Redactor redactor_;
};

} // namespace lgc
