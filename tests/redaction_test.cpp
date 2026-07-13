#include "foundation/event/memory_event_sink.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/observability/tracing.hpp"
#include "foundation/redaction/redactor.hpp"
#include "foundation/serialization/state_codec.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class MemoryLogger final : public lc::ILogger {
public:
    void log(const lc::LogRecord& record) noexcept override
    {
        records_.push_back(record);
    }

    lc::Status flush() override { return lc::Status::ok(); }
    lc::Status close() override { return lc::Status::ok(); }
    bool isClosed() const noexcept override { return false; }

    std::vector<lc::LogRecord> records_;
};

} // namespace

int main()
{
    const lc::Redactor redactor;

    {
        const auto text = redactor.redact(
            "Authorization: Bearer abcdefghijklmnop email=user@example.com key=sk-1234567890abcdef");
        assert(text.find("abcdefghijklmnop") == std::string::npos);
        assert(text.find("user@example.com") == std::string::npos);
        assert(text.find("sk-1234567890abcdef") == std::string::npos);
        assert(text.find("[REDACTED]") != std::string::npos);
    }

    {
        auto config = lc::RedactionConfig::defaults();
        config.maxStringLengthToScan_ = 8;
        const lc::Redactor limited(config);
        const auto text = limited.redact(std::string(64, 'x') + " sk-1234567890abcdef");
        assert(text == "[REDACTED]");
        assert(text.find("sk-1234567890abcdef") == std::string::npos);
    }

    {
        const nlohmann::json input = {
            { "api_key", "sk-live-1234567890" },
            { "safe", "hello" },
            { "nested", {
                            { "clientSecret", "secret-value" },
                            { "message", "contact admin@example.com" },
                        } },
        };
        const auto redacted = redactor.redact(input);
        assert(redacted["api_key"] == "[REDACTED]");
        assert(redacted["nested"]["clientSecret"] == "[REDACTED]");
        assert(redacted["nested"]["message"].get<std::string>().find("admin@example.com") == std::string::npos);
        assert(redacted["safe"] == "hello");
    }

    {
        auto config = lc::RedactionConfig::defaults();
        config.maxArraySize_ = 2;
        const lc::Redactor limited(config);
        const auto redacted = limited.redact(nlohmann::json::array({ 1, 2, 3 }));
        assert(redacted == "[REDACTED]");
    }

    {
        auto config = lc::RedactionConfig::defaults();
        config.maxObjectSize_ = 1;
        const lc::Redactor limited(config);
        const auto redacted = limited.redact(nlohmann::json {
            { "a", 1 },
            { "b", 2 },
        });
        assert(redacted == "[REDACTED]");
    }

    {
        auto config = lc::RedactionConfig::defaults();
        config.maxJsonNodes_ = 2;
        const lc::Redactor limited(config);
        const auto redacted = limited.redact(nlohmann::json {
            { "outer", {
                           { "inner", "visible" },
                       } },
        });
        assert(redacted.dump().find("visible") == std::string::npos);
    }

    {
        auto inner = std::make_shared<lc::MemoryEventSink>();
        lc::RedactionEventSink sink(inner);

        auto event = lc::RuntimeEvent::create(lc::RuntimeEventType::ToolCallStarted);
        event.message_ = "calling with token=sk-1234567890abcdef";
        event.payload_ = nlohmann::json {
            { "authorization", "Bearer abcdefghijklmnop" },
            { "input", "normal" },
        };

        assert(sink.publish(std::move(event)).isOk());
        const auto events = inner->events();
        assert(events.size() == 1);
        assert(events[0].message_.find("sk-1234567890abcdef") == std::string::npos);
        assert(events[0].payload_["authorization"] == "[REDACTED]");
        assert(events[0].payload_["input"] == "normal");
    }

    {
        auto inner = std::make_shared<lc::InMemoryTraceSink>();
        lc::RedactionTraceSink sink(inner);
        auto context = lc::makeRootContext();
        assert(context.isOk());

        lc::SpanRecord span {
            .context_ = std::move(*context),
            .name_ = "node",
            .attributes_ = nlohmann::json {
                { "password", "hunter2" },
                { "user", "user@example.com" },
            },
            .events_ = {
                lc::SpanEvent {
                    .name_ = "request",
                    .attributes_ = nlohmann::json {
                        { "headers", {
                                         { "Authorization", "Bearer abcdefghijklmnop" },
                                     } },
                    },
                },
            },
            .status_ = lc::SpanStatus::Error,
            .statusMessage_ = "failed with token=sk-1234567890abcdef",
        };

        assert(sink.recordSpan(std::move(span)).isOk());
        const auto spans = inner->spans();
        assert(spans.size() == 1);
        assert(spans[0].attributes_["password"] == "[REDACTED]");
        assert(spans[0].attributes_["user"].get<std::string>().find("user@example.com") == std::string::npos);
        assert(spans[0].events_[0].attributes_["headers"]["Authorization"] == "[REDACTED]");
        assert(spans[0].statusMessage_.find("sk-1234567890abcdef") == std::string::npos);
    }

    {
        auto inner = std::make_shared<MemoryLogger>();
        lc::RedactionLogger logger(inner);
        logger.log(lc::LogRecord {
            .level_ = lc::LogLevel::Info,
            .tag_ = "test",
            .message_ = "api key sk-1234567890abcdef belongs to user@example.com",
            .fields_ = {
                { "authorization", "Bearer abcdefghijklmnop" },
            },
        });

        assert(inner->records_.size() == 1);
        assert(inner->records_[0].message_.find("sk-1234567890abcdef") == std::string::npos);
        assert(inner->records_[0].message_.find("user@example.com") == std::string::npos);
        assert(inner->records_[0].fields_.at("authorization") == "[REDACTED]");
    }

    {
        auto state = lc::State::fromJson(R"({
            "messages":[{"role":"user","content":"email me at user@example.com"}],
            "api_key":"sk-1234567890abcdef"
        })");
        assert(state.isOk());

        lc::Checkpoint checkpoint {
            .threadId_ = "thread-1",
            .checkpointId_ = "checkpoint-1",
            .step_ = 1,
            .state_ = *state,
            .writes_ = {
                lc::CheckpointWrite {
                    .nodeId_ = "node",
                    .update_ = *lc::State::fromJson(R"({"token":"secret-token"})"),
                },
            },
        };

        lc::RedactionCheckpointCodec codec(std::make_shared<lc::JsonCheckpointCodec>());
        const auto encoded = codec.encode(checkpoint);
        assert(encoded.isOk());
        assert(encoded->data_.find("sk-1234567890abcdef") == std::string::npos);
        assert(encoded->data_.find("user@example.com") == std::string::npos);
        assert(encoded->data_.find("secret-token") == std::string::npos);

        const auto decoded = lc::JsonCheckpointCodec().decode(*encoded);
        assert(decoded.isOk());
        assert(decoded->state_.json().find("[REDACTED]") != std::string::npos);

        lc::CheckpointWrite write {
            .nodeId_ = "node",
            .update_ = *lc::State::fromJson(R"({"token":"secret-token","email":"user@example.com"})"),
            .nextTasks_ = {
                lc::CheckpointTask {
                    .nodeId_ = "next",
                    .state_ = *lc::State::fromJson(R"({"api_key":"sk-1234567890abcdef"})"),
                    .metadata_ = { { "authorization", "Bearer abcdefghijklmnop" } },
                },
            },
        };
        const auto encodedWrite = codec.encodeWrite(write);
        assert(encodedWrite.isOk());
        assert(encodedWrite->data_.find("secret-token") == std::string::npos);
        assert(encodedWrite->data_.find("user@example.com") == std::string::npos);
        assert(encodedWrite->data_.find("sk-1234567890abcdef") == std::string::npos);

        const auto decodedWrite = lc::JsonCheckpointCodec().decodeWrite(*encodedWrite);
        assert(decodedWrite.isOk());
        assert(decodedWrite->update_.json().find("[REDACTED]") != std::string::npos);
    }

    return 0;
}
