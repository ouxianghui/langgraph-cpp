#include <langgraph_cpp/langgraph.hpp>

#include "foundation/network/sse_parser.hh"
#include "foundation/process/process.hpp"
#include "foundation/serialization/content_envelope.hpp"
#include "langgraph/graph/graph_namespace.hh"
#include "langgraph/graph/run_config.hpp"

#if LANGGRAPH_CPP_WITH_SQLITE
#include "foundation/storage/sqlite_storage.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;

[[nodiscard]] lgc::State stateFromJson(std::string text)
{
    auto state = lgc::State::fromJson(std::move(text));
    assert(state.isOk());
    return std::move(*state);
}

[[nodiscard]] lgc::State stateFromJsonValue(Json value)
{
    auto state = lgc::State::fromJsonValue(value);
    assert(state.isOk());
    return std::move(*state);
}

[[nodiscard]] lgc::StateUpdate updateFromJsonValue(Json value)
{
    auto update = lgc::StateUpdate::fromJsonValue(value);
    assert(update.isOk());
    return std::move(*update);
}

[[nodiscard]] std::string padded(std::uint64_t value, std::size_t width = 6)
{
    auto text = std::to_string(value);
    if (text.size() >= width)
        return text;
    return std::string(width - text.size(), '0') + text;
}

[[nodiscard]] std::string deterministicText(std::size_t bytes, char seed)
{
    std::string out;
    out.reserve(bytes);
    for (std::size_t i = 0; i < bytes; ++i)
        out.push_back(static_cast<char>('a' + ((seed + static_cast<char>(i)) % 26)));
    return out;
}

[[nodiscard]] lgc::CheckpointWrite checkpointWrite(
    std::uint64_t order,
    std::string node = "node",
    Json update = Json::object())
{
    if (update.empty())
        update = Json { { "value", order } };
    return lgc::CheckpointWrite {
        .taskId_ = "task-" + std::to_string(order % 17U),
        .taskPath_ = "root/task-" + std::to_string(order % 7U),
        .nodeId_ = std::move(node),
        .checkpointNamespace_ = "root",
        .update_ = stateFromJsonValue(std::move(update)),
        .order_ = order,
        .metadata_ = {
            { "order", order },
        },
    };
}

[[nodiscard]] lgc::Checkpoint checkpointFor(
    std::string threadId,
    std::string checkpointNamespace,
    std::string checkpointId,
    std::uint64_t step,
    Json state = Json::object())
{
    if (state.empty())
        state = Json { { "step", step } };
    return lgc::Checkpoint {
        .threadId_ = std::move(threadId),
        .checkpointId_ = std::move(checkpointId),
        .checkpointNamespace_ = std::move(checkpointNamespace),
        .parentCheckpointId_ = step == 0U ? std::nullopt : std::optional<std::string>("cp-" + padded(step - 1U)),
        .step_ = step,
        .state_ = stateFromJsonValue(std::move(state)),
        .nextNodes_ = { step % 2U == 0U ? "next" : std::string(lgc::END) },
        .nextTasks_ = {
            lgc::CheckpointTask {
                .taskId_ = "task-next-" + std::to_string(step),
                .nodeId_ = "next",
                .checkpointNamespace_ = "root",
                .state_ = stateFromJsonValue({ { "task_step", step } }),
                .order_ = step,
                .metadata_ = { { "source", "fuzz" } },
            },
        },
        .writes_ = {
            checkpointWrite(step, "writer", { { "write", step } }),
        },
        .channelVersions_ = {
            { "value", step },
        },
        .versionsSeen_ = {
            { "writer", { { "value", step } } },
        },
        .updatedChannels_ = { "value" },
        .metadata_ = {
            { "source", "fuzz-pressure" },
            { "step", step },
        },
        .createdAt_ = std::chrono::system_clock::time_point(std::chrono::milliseconds(step)),
    };
}

void fuzzJsonSchemaParserValidator()
{
    const lgc::SchemaValidator validator;
    std::mt19937 rng(0x5C4E'0001U);

    for (int i = 0; i < 384; ++i) {
        const int selector = i % 8;
        if (selector == 0) {
            auto schema = lgc::JsonSchema::fromJson(Json {
                { "type", "object" },
                { "required", Json::array({ "id", "score" }) },
                { "properties", {
                                    { "id", { { "type", "string" }, { "pattern", "^item-[0-9]+$" } } },
                                    { "score", { { "type", "number" }, { "minimum", 0 }, { "maximum", 1 } } },
                                    { "tags", { { "type", "array" }, { "items", { { "type", "string" } } }, { "maxItems", 4 } } },
                                } },
                { "additionalProperties", false },
            });
            assert(schema.isOk());
            assert(validator.check(Json {
                       { "id", "item-" + std::to_string(i) },
                       { "score", static_cast<double>(rng() % 100U) / 100.0 },
                       { "tags", Json::array({ "a", "b" }) },
                   },
                       *schema)
                       .isOk());
            assert(!validator.check(Json {
                        { "id", "bad" },
                        { "score", 2 },
                        { "extra", true },
                    },
                        *schema)
                        .isOk());
        } else if (selector == 1) {
            auto schema = lgc::JsonSchema::array()
                              .items(lgc::JsonSchema::integer().minimum(-10).maximum(10))
                              .minItems(1)
                              .maxItems(5)
                              .uniqueItems();
            assert(validator.check(Json::array({ -1, 0, 1 }), schema).isOk());
            assert(!validator.check(Json::array({ 1, 1 }), schema).isOk());
            assert(!validator.check(Json::array({ 11 }), schema).isOk());
        } else if (selector == 2) {
            auto schema = lgc::JsonSchema::string()
                              .minLength(3)
                              .maxLength(12)
                              .pattern(R"(^[a-z]+-[0-9]+$)");
            assert(validator.check("abc-" + std::to_string(i), schema).isOk());
            assert(!validator.check("ABC-" + std::to_string(i), schema).isOk());
        } else if (selector == 3) {
            auto schema = lgc::JsonSchema::fromJson(Json {
                { "anyOf", Json::array({
                               Json { { "type", "boolean" } },
                               Json { { "type", "integer" }, { "multipleOf", 2 } },
                           }) },
            });
            assert(schema.isOk());
            assert(validator.check(true, *schema).isOk());
            assert(validator.check(4, *schema).isOk());
            assert(!validator.check(5, *schema).isOk());
        } else if (selector == 4) {
            auto schema = lgc::JsonSchema::fromJson(Json {
                { "oneOf", Json::array({
                               Json { { "type", "integer" } },
                               Json { { "minimum", 0 } },
                           }) },
            });
            assert(schema.isOk());
            assert(validator.check(-1, *schema).isOk());
            assert(!validator.check(1, *schema).isOk());
        } else if (selector == 5) {
            auto schema = lgc::JsonSchema::fromJson(Json {
                { "required", Json::array({ "a", "a" }) },
            });
            assert(!schema.isOk());
            assert(schema.status().code() == lgc::StatusCode::InvalidArgument);
        } else if (selector == 6) {
            auto schema = lgc::JsonSchema::fromJson(Json {
                { "type", "string" },
                { "pattern", "[" },
            });
            assert(!schema.isOk());
            assert(schema.status().code() == lgc::StatusCode::InvalidArgument);
        } else {
            auto schema = lgc::JsonSchema::fromJson(Json {
                { "type", "object" },
                { "unknownKeyword", true },
            });
            assert(!schema.isOk());
            auto relaxed = lgc::JsonSchema::fromJson(
                Json {
                    { "type", "object" },
                    { "unknownKeyword", true },
                },
                lgc::JsonSchemaOptions {
                    .allowUnknownKeywords_ = true,
                });
            assert(relaxed.isOk());
        }
    }

    auto limited = validator.validateText(
        R"({"items":[{"name":"tool"},{"name":"runtime"}]})",
        lgc::JsonSchema::object().property(
            "items",
            lgc::JsonSchema::array().items(lgc::JsonSchema::object()),
            true),
        lgc::ValidationOptions {
            .maxDepth_ = 1,
        });
    assert(limited.isOk());
    assert(!limited->isValid());
    assert(limited->errors().front().message_.find("max depth") != std::string::npos);
}

void fuzzStateUpdateReducerMerge()
{
    lgc::ReducerRegistry reducers;
    reducers
        .set("numbers", lgc::ReducerKind::Append)
        .set("messages", lgc::ReducerKind::AddMessages)
        .set("metadata", lgc::ReducerKind::MergeObject)
        .set("sum", [](const Json& left, const Json& right) -> lgc::Result<Json> {
            const auto lhs = left.is_null() ? 0 : left.get<int>();
            return Json(lhs + right.get<int>());
        });

    auto state = stateFromJson(R"({"numbers":[],"messages":[],"metadata":{},"sum":0,"status":"new"})");
    for (int i = 0; i < 512; ++i) {
        auto message = lgc::BaseMessage::human("message-" + std::to_string(i));
        message.id_ = "msg-" + std::to_string(i % 64);

        auto merged = lgc::applyStateUpdate(
            state,
            updateFromJsonValue({
                { "numbers", Json::array({ i }) },
                { "messages", Json::array({ lgc::baseMessageToJson(message) }) },
                { "metadata", { { "k" + std::to_string(i % 23), i } } },
                { "sum", 1 },
                { "status", i % 2 == 0 ? "even" : "odd" },
            }),
            reducers);
        assert(merged.isOk());
        state = std::move(*merged);
    }

    const auto& view = state.view();
    assert(view.at("numbers").size() == 512);
    assert(view.at("metadata").size() == 23);
    assert(view.at("sum") == 512);
    assert(view.at("messages").size() == 64);
    assert(view.at("messages").back().at("content") == "message-511");

    auto overwritten = lgc::applyStateUpdate(
        state,
        updateFromJsonValue({
            { "numbers", lgc::overwriteToJson(lgc::Overwrite { .value_ = Json::array({ "reset" }) }) },
        }),
        reducers);
    assert(overwritten.isOk());
    assert(overwritten->view().at("numbers") == Json::array({ "reset" }));

    auto invalidMessages = lgc::applyStateUpdate(
        stateFromJson(R"({"messages":"not-array"})"),
        updateFromJsonValue({ { "messages", Json::array({ "x" }) } }),
        reducers);
    assert(!invalidMessages.isOk());
    assert(invalidMessages.status().code() == lgc::StatusCode::InvalidArgument);
}

void fuzzCheckpointCodec()
{
    const lgc::JsonCheckpointCodec codec;

    for (std::uint64_t i = 0; i < 256; ++i) {
        auto checkpoint = checkpointFor(
            "codec-thread-" + std::to_string(i % 5U),
            i % 2U == 0U ? "root" : "root|child",
            "cp-" + padded(i),
            i,
            Json {
                { "value", i },
                { "text", deterministicText(32 + (i % 32U), static_cast<char>(i)) },
            });
        checkpoint.pendingWrites_.push_back(checkpointWrite(i + 1000U, "pending-a"));
        checkpoint.pendingWrites_.push_back(checkpointWrite(i + 2000U, "pending-b"));

        auto encoded = codec.encode(checkpoint);
        assert(encoded.isOk());
        assert(lgc::isCheckpointPayload(encoded->contentType_));
        auto decoded = codec.decode(*encoded);
        assert(decoded.isOk());
        assert(decoded->threadId_ == checkpoint.threadId_);
        assert(decoded->checkpointId_ == checkpoint.checkpointId_);
        assert(decoded->checkpointNamespace_ == checkpoint.checkpointNamespace_);
        assert(decoded->step_ == checkpoint.step_);
        assert(decoded->state_ == checkpoint.state_);
        assert(decoded->pendingWrites_ == checkpoint.pendingWrites_);
        assert(decoded->createdAt_ == checkpoint.createdAt_);

        auto encodedWrite = codec.encodeWrite(checkpoint.pendingWrites_.front());
        assert(encodedWrite.isOk());
        assert(lgc::isCheckpointWritePayload(encodedWrite->contentType_));
        auto decodedWrite = codec.decodeWrite(*encodedWrite);
        assert(decodedWrite.isOk());
        assert(*decodedWrite == checkpoint.pendingWrites_.front());
    }

    auto wrongType = codec.decode(lgc::Payload {
        .contentType_ = "application/json",
        .data_ = "{}",
    });
    assert(!wrongType.isOk());
    assert(wrongType.status().code() == lgc::StatusCode::InvalidArgument);

    auto unknownField = codec.decode(lgc::Payload {
        .contentType_ = "application/vnd.langgraph-cpp.checkpoint+json",
        .data_ = R"({"thread_id":"t","checkpoint_id":"c","step":1,"state":{},"unexpected":true})",
    });
    assert(!unknownField.isOk());
    assert(unknownField.status().code() == lgc::StatusCode::InvalidArgument);
}

void fuzzContentEnvelope()
{
    lgc::EnvelopeCodec envelope;
    lgc::EnvelopeOptions options;
#if !LANGGRAPH_CPP_WITH_CRYPTO
    options.checksum_ = false;
#endif

    for (int i = 0; i < 128; ++i) {
        lgc::Payload payload {
            .contentType_ = "application/json",
            .data_ = Json {
                { "index", i },
                { "text", deterministicText(64 + static_cast<std::size_t>(i % 64), static_cast<char>(i)) },
            }.dump(),
        };

        auto encoded = envelope.encode(payload, options);
        assert(encoded.isOk());
        assert(encoded->contentType_ == lgc::envelopeContentType());

        auto decoded = envelope.decode(*encoded, options);
        assert(decoded.isOk());
        assert(decoded->contentType_ == payload.contentType_);
        assert(decoded->data_ == payload.data_);

        auto parsed = lgc::deserializeEnvelope(encoded->data_);
        assert(parsed.isOk());
        auto serialized = lgc::serializeEnvelope(*parsed);
        assert(serialized.isOk());
        auto reparsed = lgc::deserializeEnvelope(*serialized);
        assert(reparsed.isOk());
        assert(reparsed->contentType_ == payload.contentType_);
    }

    auto checkpoint = checkpointFor("envelope-thread", "root", "cp-envelope", 9);
    checkpoint.pendingWrites_.push_back(checkpointWrite(9));
    lgc::EnvelopedCheckpointCodec codec(std::make_shared<lgc::JsonCheckpointCodec>(), envelope, options);
    auto encodedCheckpoint = codec.encode(checkpoint);
    assert(encodedCheckpoint.isOk());
    auto decodedCheckpoint = codec.decode(*encodedCheckpoint);
    assert(decodedCheckpoint.isOk());
    assert(decodedCheckpoint->checkpointId_ == checkpoint.checkpointId_);
    assert(decodedCheckpoint->pendingWrites_ == checkpoint.pendingWrites_);

    auto encodedWrite = codec.encodeWrite(checkpoint.pendingWrites_.front());
    assert(encodedWrite.isOk());
    auto decodedWrite = codec.decodeWrite(*encodedWrite);
    assert(decodedWrite.isOk());
    assert(*decodedWrite == checkpoint.pendingWrites_.front());

    auto malformed = lgc::deserializeEnvelope(R"({"version":1,"content_type":"application/json","data_hex":"00","extra":true})");
    assert(!malformed.isOk());
    assert(malformed.status().code() == lgc::StatusCode::InvalidArgument);
}

void fuzzSseParser()
{
    std::mt19937 rng(0x55E'0002U);

    for (int i = 0; i < 160; ++i) {
        const std::string eventName = i % 3 == 0 ? "token" : "";
        const std::string id = "evt-" + std::to_string(i);
        const std::string first = "hello-" + std::to_string(i);
        const std::string second = "world-" + std::to_string(i);
        std::string stream;
        stream += ": ignored comment\n";
        if (!eventName.empty())
            stream += "event: " + eventName + "\n";
        stream += "data: " + first + "\n";
        stream += "data: " + second + "\n";
        stream += "id: " + id + "\n";
        if (i % 2 == 0)
            stream += "retry: " + std::to_string(10 + i) + "\n";
        else
            stream += "retry: not-a-number\n";
        stream += "\n";

        lgc::http_client_detail::SseParser parser;
        std::vector<lgc::ServerSentEvent> events;
        std::size_t offset = 0;
        while (offset < stream.size()) {
            const auto chunk = 1U + (rng() % 7U);
            const auto size = std::min<std::size_t>(chunk, stream.size() - offset);
            auto status = parser.feed(std::string_view(stream).substr(offset, size), [&](const lgc::ServerSentEvent& event) {
                events.push_back(event);
                return lgc::Status::ok();
            });
            assert(status.isOk());
            offset += size;
        }
        assert(parser.finish([&](const lgc::ServerSentEvent& event) {
            events.push_back(event);
            return lgc::Status::ok();
        }).isOk());

        assert(events.size() == 1);
        assert(events.front().event_ == (eventName.empty() ? "message" : eventName));
        assert(events.front().data_ == first + "\n" + second);
        assert(events.front().id_ == id);
        if (i % 2 == 0) {
            assert(events.front().retry_.has_value());
            assert(*events.front().retry_ == std::chrono::milliseconds(10 + i));
        } else {
            assert(!events.front().retry_.has_value());
        }
    }

    lgc::http_client_detail::SseParser parser;
    auto cancelled = parser.feed("data: stop\n\n", [](const lgc::ServerSentEvent&) {
        return lgc::Status::cancelled("consumer stopped");
    });
    assert(!cancelled.isOk());
    assert(cancelled.code() == lgc::StatusCode::Cancelled);
}

void fuzzMessageToolCallJsonParser()
{
    for (int i = 0; i < 256; ++i) {
        lgc::BaseMessage message;
        switch (i % 4) {
        case 0:
            message = lgc::BaseMessage::system("system-" + std::to_string(i));
            break;
        case 1:
            message = lgc::BaseMessage::human("human-" + std::to_string(i));
            message.contentBlocks_ = Json::array({
                Json { { "type", "text" }, { "text", message.content_ } },
                Json { { "type", "image" }, { "url", "https://example.com/" + std::to_string(i) + ".jpg" } },
            });
            break;
        case 2:
            message = lgc::BaseMessage::ai(
                "ai-" + std::to_string(i),
                {
                    lgc::ToolCall {
                        .id_ = "call-" + std::to_string(i),
                        .name_ = "tool_" + std::to_string(i % 9),
                        .args_ = {
                            { "value", i },
                        },
                    },
                });
            message.usageMetadata_.source_ = lgc::UsageMetadataSource::Provider;
            message.usageMetadata_.provider_ = "fuzz";
            message.usageMetadata_.tokens_.inputTokens_ = static_cast<std::uint64_t>(i);
            message.usageMetadata_.tokens_.outputTokens_ = static_cast<std::uint64_t>(i + 1);
            message.usageMetadata_.tokens_.totalTokens_ = static_cast<std::uint64_t>(i * 2 + 1);
            message.responseMetadata_ = { { "provider", "fuzz" } };
            break;
        default:
            message = lgc::BaseMessage::tool(
                "call-" + std::to_string(i),
                "tool_" + std::to_string(i % 9),
                Json { { "ok", true }, { "index", i } }.dump());
            message.artifact_ = { { "bytes", i } };
            break;
        }
        message.id_ = "msg-" + std::to_string(i);

        auto encoded = lgc::baseMessageToJson(message);
        auto decoded = lgc::baseMessageFromJson(encoded);
        assert(decoded.isOk());
        assert(decoded->id_ == message.id_);
        assert(decoded->type_ == message.type_);
        assert(decoded->content_ == message.content_);
        assert(decoded->toolCalls_ == message.toolCalls_);
        assert(decoded->toolCallId_ == message.toolCallId_);
        assert(decoded->name_ == message.name_);
    }

    auto nativeBlocks = lgc::normalizeContentBlocks(Json::array({
        Json {
            { "type", "image_url" },
            { "image_url", {
                               { "url", "https://example.com/native.jpg" },
                               { "detail", "low" },
                           } },
        },
        Json {
            { "type", "input_audio" },
            { "input_audio", {
                                 { "data", "AAAA" },
                                 { "format", "wav" },
                             } },
        },
        Json {
            { "type", "tool_call_chunk" },
            { "id", "chunk-1" },
            { "name", "lookup" },
            { "args", R"({"q":"edge"})" },
            { "index", std::uint64_t { 0 } },
        },
    }));
    assert(nativeBlocks.isOk());
    assert(nativeBlocks->at(0).at("type") == "image");
    assert(nativeBlocks->at(0).at("extras").at("detail") == "low");
    assert(nativeBlocks->at(1).at("type") == "audio");
    assert(nativeBlocks->at(1).at("mime_type") == "audio/wav");

    auto messages = lgc::messagesFromJson(lgc::messagesToJson({
        lgc::BaseMessage::system("sys"),
        lgc::BaseMessage::human("hi"),
        lgc::BaseMessage::ai("answer"),
    }));
    assert(messages.isOk());
    assert(messages->size() == 3);

    auto invalidToolCall = lgc::toolCallFromJson(Json {
        { "id", "call" },
        { "name", "tool" },
        { "args", Json::array() },
    });
    assert(!invalidToolCall.isOk());
    assert(invalidToolCall.status().code() == lgc::StatusCode::InvalidArgument);

    auto toolResult = lgc::ToolResult::failure(
        lgc::ToolErrorCode::Rejected,
        "blocked",
        { { "reason", "policy" } });
    auto decodedToolResult = lgc::toolResultFromJson(lgc::toolResultToJson(toolResult));
    assert(decodedToolResult.isOk());
    assert(!decodedToolResult->ok_);
    assert(decodedToolResult->error_.has_value());
    assert(decodedToolResult->error_->code_ == lgc::ToolErrorCode::Rejected);
}

void fuzzGraphNamespaceParser()
{
    std::mt19937 rng(0xC0DE'0003U);
    for (int i = 0; i < 512; ++i) {
        std::vector<std::string> expected;
        std::string nameSpace;
        const int segments = 1 + static_cast<int>(rng() % 6U);
        if (i % 5 == 0)
            nameSpace.push_back(lgc::detail::kCheckpointNamespaceSeparator);
        for (int s = 0; s < segments; ++s) {
            if (s > 0 || !nameSpace.empty())
                nameSpace.push_back(lgc::detail::kCheckpointNamespaceSeparator);
            if (i % 7 == 0 && s == 1)
                continue;
            auto segment = "node-" + std::to_string(i) + "-" + std::to_string(s);
            expected.push_back(segment);
            nameSpace.append(segment);
        }
        if (i % 3 == 0)
            nameSpace.push_back(lgc::detail::kCheckpointNamespaceSeparator);

        auto parsed = lgc::detail::namespacePathFromString(nameSpace);
        assert(parsed.is_array());
        assert(parsed.size() == expected.size());
        for (std::size_t n = 0; n < expected.size(); ++n)
            assert(parsed.at(n) == expected.at(n));
    }

    auto root = lgc::detail::namespacePathFromString("");
    assert(root.is_array());
    assert(root.empty());
}

void fuzzRunnableConfigMergePatch()
{
    auto config = lgc::RunnableConfig::fromJson(Json {
        { "tags", Json::array({ "base" }) },
        { "metadata", { { "source", "test" } } },
        { "configurable", {
                              { "thread_id", "thread-a" },
                              { "checkpoint_ns", "root" },
                              { "assistant_id", "assistant-a" },
                          } },
        { "run_name", "base-run" },
    });
    assert(config.isOk());
    assert(config->metadata_.at("thread_id") == "thread-a");
    assert(config->metadata_.at("checkpoint_ns") == "root");
    assert(config->metadata_.at("assistant_id") == "assistant-a");

    for (int i = 0; i < 128; ++i) {
        auto patched = lgc::patchRunnableConfig(
            *config,
            Json {
                { "tags", Json::array({ "tag-" + std::to_string(i) }) },
                { "metadata", { { "m" + std::to_string(i % 11), i } } },
                { "configurable", { { "c" + std::to_string(i % 13), i } } },
                { "extra_" + std::to_string(i % 17), i },
                { "max_concurrency", 1 + (i % 8) },
            });
        assert(patched.isOk());
        config = std::move(patched);
    }

    assert(config->tags_.size() == 129);
    assert(config->metadata_.at("m0").is_number_integer());
    assert(config->configurable_.at("c0").is_number_integer());
    assert(config->configurable_.contains("extra_0"));
    assert(config->maxConcurrency_.has_value());

    auto overrideConfig = lgc::RunnableConfig::fromJson(Json {
        { "tags", Json::array({ "override" }) },
        { "metadata", { { "source", "override" }, { "new", true } } },
        { "configurable", { { "thread_id", "thread-b" }, { "checkpoint_ns", "child" } } },
        { "run_id", "run-override" },
    });
    assert(overrideConfig.isOk());

    auto merged = lgc::mergeRunnableConfigs({ *config, *overrideConfig });
    assert(merged.isOk());
    assert(merged->tags_.back() == "override");
    assert(merged->metadata_.at("source") == "override");
    assert(merged->metadata_.at("new") == true);
    assert(merged->configurable_.at("thread_id") == "thread-b");
    assert(merged->configurable_.at("checkpoint_ns") == "child");
    assert(merged->runId_ == "run-override");

    lgc::RunOptions options;
    options.threadId_ = "original";
    options.checkpointNamespace_ = "original-ns";
    auto applied = lgc::applyRunnableConfig(std::move(options), *merged);
    assert(applied.isOk());
    assert(applied->threadId_ == "thread-b");
    assert(applied->checkpointNamespace_ == "child");
    assert(applied->runId_ == "run-override");
    assert(applied->maxConcurrency_ == *merged->maxConcurrency_);

    auto invalid = lgc::patchRunnableConfig(*merged, Json::array());
    assert(!invalid.isOk());
    assert(invalid.status().code() == lgc::StatusCode::InvalidArgument);
}

void stressCheckpointHistory1000()
{
    auto exercise = [](lgc::BaseCheckpointSaver& saver, std::string threadId) {
        constexpr std::uint64_t kCheckpoints = 1008;
        for (std::uint64_t step = 0; step < kCheckpoints; ++step) {
            auto checkpoint = checkpointFor(
                threadId,
                "root",
                "cp-" + padded(step),
                step,
                Json {
                    { "step", step },
                    { "payload", deterministicText(32, static_cast<char>(step)) },
                });
            assert(saver.put(std::move(checkpoint)).isOk());
        }

        auto latest = saver.getTuple(lgc::CheckpointQuery::latest(threadId, "root"));
        assert(latest.isOk());
        assert(latest->has_value());
        assert((*latest)->checkpoint_.checkpointId_ == "cp-" + padded(kCheckpoints - 1U));
        assert((*latest)->checkpoint_.state_.view().at("step") == kCheckpoints - 1U);

        auto history = saver.list(lgc::CheckpointListOptions {
            .threadId_ = threadId,
            .checkpointNamespace_ = std::string("root"),
            .order_ = lgc::CheckpointListOrder::OldestFirst,
        });
        assert(history.isOk());
        assert(history->size() == kCheckpoints);
        assert(history->front().checkpoint_.checkpointId_ == "cp-" + padded(0));
        assert(history->back().checkpoint_.checkpointId_ == "cp-" + padded(kCheckpoints - 1U));

        auto newestLimited = saver.list(lgc::CheckpointListOptions {
            .threadId_ = threadId,
            .checkpointNamespace_ = std::string("root"),
            .limit_ = 7,
        });
        assert(newestLimited.isOk());
        assert(newestLimited->size() == 7);
        assert(newestLimited->front().checkpoint_.checkpointId_ == "cp-" + padded(kCheckpoints - 1U));

        auto pruned = saver.prune(
            threadId,
            lgc::CheckpointPruneOptions {
                .checkpointNamespace_ = "root",
                .keepLatest_ = 8,
            });
        assert(pruned.isOk());
        assert(pruned->removed_ == kCheckpoints - 8U);
        assert(pruned->remaining_ == 8U);
        assert(pruned->latestCheckpointId_ == "cp-" + padded(kCheckpoints - 1U));
        assert(saver.deleteThread(threadId).isOk());
    };

    lgc::InMemorySaver memory;
    exercise(memory, "history-memory");

    auto storage = std::make_shared<lgc::MemoryStorage>();
    lgc::StorageSaver storageSaver(storage, lgc::StorageSaverOptions { .listPageSize_ = 37 });
    exercise(storageSaver, "history-storage");
}

[[nodiscard]] lgc::CompiledStateGraph buildIncrementGraph()
{
    lgc::StateGraph graph;
    assert(graph.addNode("inc", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        const auto next = state.view().value("value", 0) + 1;
        return lgc::StateUpdate::fromJsonValue({ { "value", next } });
    }).isOk());
    assert(graph.addEdge(std::string(lgc::START), "inc").isOk());
    assert(graph.addEdge("inc", std::string(lgc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

void stressConcurrentGraphInvocations1000()
{
    const auto compiled = buildIncrementGraph();
    auto checkpointer = std::make_shared<lgc::InMemorySaver>();

    constexpr int kWorkers = 32;
    constexpr int kPerWorker = 32;
    std::mutex mutex;
    std::condition_variable cv;
    bool start = false;
    int ready = 0;
    std::atomic<int> completed { 0 };

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            {
                std::unique_lock lock(mutex);
                ++ready;
                cv.notify_all();
                cv.wait(lock, [&] { return start; });
            }
            for (int index = 0; index < kPerWorker; ++index) {
                lgc::RunOptions options;
                options.threadId_ = "concurrent-" + std::to_string(worker) + "-" + std::to_string(index);
                options.checkpointer_ = checkpointer;
                auto result = compiled.invoke(stateFromJsonValue({ { "value", index } }), options);
                assert(result.isOk());
                assert(result->state_.view().at("value") == index + 1);
                completed.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }

    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return ready == kWorkers; }));
        start = true;
    }
    cv.notify_all();

    for (auto& worker : workers)
        worker.join();
    assert(completed.load(std::memory_order_acquire) == kWorkers * kPerWorker);

    auto latest = checkpointer->getTuple(lgc::CheckpointQuery::latest("concurrent-0-0"));
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->checkpoint_.state_.view().at("value") == 1);
}

[[nodiscard]] lgc::CompiledStateGraph buildInterruptGraph()
{
    lgc::StateGraph graph;
    assert(graph.addNode("gate", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::NodeOutput> {
        if (!context.hasResumeValue()) {
            return lgc::NodeOutput::interrupt(lgc::Interrupt {
                .id_ = "approval",
                .value_ = { { "question", "continue?" } },
            });
        }
        auto update = lgc::StateUpdate::fromJsonValue({
            { "approved", context.resumeValue().value("approved", false) },
            { "worker", context.resumeValue().value("worker", -1) },
        });
        if (!update.isOk())
            return update.status();
        return lgc::NodeOutput::update(std::move(*update));
    }).isOk());
    assert(graph.addEdge(std::string(lgc::START), "gate").isOk());
    assert(graph.addEdge("gate", std::string(lgc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

void stressMultithreadStreamConsumerResume()
{
    const auto compiled = buildInterruptGraph();
    auto checkpointer = std::make_shared<lgc::InMemorySaver>();

    constexpr int kWorkers = 16;
    std::atomic<int> completed { 0 };
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);

    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker] {
            lgc::RunOptions options;
            options.threadId_ = "stream-resume-" + std::to_string(worker);
            options.checkpointer_ = checkpointer;

            auto streamResult = compiled.streamProjected(
                stateFromJson("{}"),
                options,
                lgc::RunProjectionOptions {
                    .modes_ = { lgc::StreamMode::Interrupts, lgc::StreamMode::Values, lgc::StreamMode::Events },
                    .capacity_ = 16,
                });
            assert(streamResult.isOk());
            auto stream = std::move(*streamResult);

            bool sawInterrupt = false;
            for (;;) {
                auto part = stream.nextFor(std::chrono::seconds(2));
                assert(part.isOk());
                if (!part->has_value())
                    break;
                sawInterrupt = sawInterrupt
                    || (*part)->mode_ == lgc::StreamMode::Interrupts
                    || ((*part)->mode_ == lgc::StreamMode::Values && (*part)->data_.contains("__interrupt__"));
            }
            auto paused = stream.result();
            assert(paused.isOk());
            assert(paused->status_ == lgc::RunStatus::Paused);
            assert(sawInterrupt);

            lgc::RunOptions resumeOptions;
            resumeOptions.checkpointer_ = checkpointer;
            resumeOptions.command_ = lgc::Command::resume({
                { "approved", true },
                { "worker", worker },
            });
            auto resumedStream = compiled.resumeProjected(
                options.threadId_,
                resumeOptions,
                lgc::RunProjectionOptions {
                    .modes_ = { lgc::StreamMode::Updates, lgc::StreamMode::Values },
                    .capacity_ = 16,
                });
            assert(resumedStream.isOk());
            auto resume = std::move(*resumedStream);
            bool sawResumeValue = false;
            for (;;) {
                auto part = resume.nextFor(std::chrono::seconds(2));
                assert(part.isOk());
                if (!part->has_value())
                    break;
                sawResumeValue = sawResumeValue
                    || ((*part)->mode_ == lgc::StreamMode::Values
                        && (*part)->data_.contains("worker")
                        && (*part)->data_.at("worker") == worker);
            }
            auto completedRun = resume.result();
            assert(completedRun.isOk());
            assert(completedRun->status_ == lgc::RunStatus::Completed);
            assert(completedRun->state_.view().at("approved") == true);
            assert(completedRun->state_.view().at("worker") == worker);
            assert(sawResumeValue);
            completed.fetch_add(1, std::memory_order_acq_rel);
        });
    }

    for (auto& worker : workers)
        worker.join();
    assert(completed.load(std::memory_order_acquire) == kWorkers);
}

#if LANGGRAPH_CPP_WITH_SQLITE

[[nodiscard]] std::filesystem::path uniqueDatabasePath(std::string_view name)
{
    return std::filesystem::temp_directory_path()
        / ("langgraph-cpp-" + std::string(name) + "-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
}

void removeDatabaseFiles(const std::filesystem::path& path)
{
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
    std::filesystem::remove(path.string() + "-journal");
}

[[nodiscard]] std::shared_ptr<lgc::SQLiteStorage> openSQLite(const std::filesystem::path& path)
{
    auto storage = std::make_shared<lgc::SQLiteStorage>(
        path.string(),
        lgc::Logger::defaultLogger(),
        lgc::StorageLimits {},
        lgc::SystemWallClock::instance(),
        lgc::SQLiteStorageOptions {
            .busyTimeout_ = std::chrono::seconds(10),
            .journalMode_ = lgc::SQLiteJournalMode::Wal,
            .synchronousMode_ = lgc::SQLiteSynchronousMode::Full,
        });
    auto opened = storage->open();
    assert(opened.isOk());
    return storage;
}

int sqliteContentionChild(
    const std::filesystem::path& databasePath,
    int writer,
    std::uint64_t checkpointCount)
{
    auto storage = openSQLite(databasePath);
    lgc::StorageSaver saver(storage, lgc::StorageSaverOptions { .listPageSize_ = 31 });
    const std::string threadId = "sqlite-contention-" + std::to_string(writer);
    for (std::uint64_t step = 0; step < checkpointCount; ++step) {
        auto checkpoint = checkpointFor(
            threadId,
            "root",
            "cp-" + padded(step),
            step,
            Json {
                { "writer", writer },
                { "step", step },
                { "payload", deterministicText(64, static_cast<char>(writer + static_cast<int>(step))) },
            });
        auto status = saver.put(std::move(checkpoint));
        assert(status.isOk());
        if (step % 17U == 0U)
            assert(storage->flush().isOk());
    }
    assert(storage->flush().isOk());
    assert(storage->close().isOk());
    return 0;
}

void stressSQLiteMultiProcessContention(const char* executable)
{
    const auto path = uniqueDatabasePath("sqlite-contention");
    removeDatabaseFiles(path);

    constexpr int kWriters = 4;
    constexpr std::uint64_t kCheckpointsPerWriter = 128;
    std::vector<std::future<void>> writers;
    writers.reserve(kWriters);
    for (int writer = 0; writer < kWriters; ++writer) {
        writers.push_back(std::async(std::launch::async, [executable, path, writer] {
            lgc::ProcessRunner runner;
            auto result = runner.run(lgc::ProcessOptions {
                .executable_ = executable,
                .arguments_ = {
                    "--sqlite-contention-child",
                    path.string(),
                    std::to_string(writer),
                    std::to_string(kCheckpointsPerWriter),
                },
                .timeout_ = std::chrono::seconds(30),
                .maxStdoutBytes_ = 64 * 1024,
                .maxStderrBytes_ = 64 * 1024,
            });
            assert(result.isOk());
            assert(result->success());
        }));
    }
    for (auto& writer : writers)
        writer.get();

    auto storage = openSQLite(path);
    lgc::StorageSaver saver(storage, lgc::StorageSaverOptions { .listPageSize_ = 29 });
    for (int writer = 0; writer < kWriters; ++writer) {
        const std::string threadId = "sqlite-contention-" + std::to_string(writer);
        auto latest = saver.getTuple(lgc::CheckpointQuery::latest(threadId, "root"));
        assert(latest.isOk());
        assert(latest->has_value());
        assert((*latest)->checkpoint_.checkpointId_ == "cp-" + padded(kCheckpointsPerWriter - 1U));
        assert((*latest)->checkpoint_.state_.view().at("writer") == writer);
        assert((*latest)->checkpoint_.state_.view().at("step") == kCheckpointsPerWriter - 1U);

        auto history = saver.list(lgc::CheckpointListOptions {
            .threadId_ = threadId,
            .checkpointNamespace_ = std::string("root"),
            .order_ = lgc::CheckpointListOrder::OldestFirst,
        });
        assert(history.isOk());
        assert(history->size() == kCheckpointsPerWriter);
    }
    assert(storage->close().isOk());
    removeDatabaseFiles(path);
}

#else

void stressSQLiteMultiProcessContention(const char*) { }

#endif

void stressLargeStateMessagePendingWrites()
{
    Json largeState = Json::object();
    for (int i = 0; i < 3000; ++i)
        largeState["field_" + padded(static_cast<std::uint64_t>(i), 4)] = i;
    auto state = stateFromJsonValue(largeState);
    assert(state.view().size() == 3000);

    Json largeUpdate = Json::object();
    for (int i = 0; i < 900; ++i)
        largeUpdate["extra_" + padded(static_cast<std::uint64_t>(i), 4)] = i;
    auto merged = lgc::applyStateUpdate(state, updateFromJsonValue(largeUpdate));
    assert(merged.isOk());
    assert(merged->view().size() == 3900);

    lgc::BaseMessage largeMessage = lgc::BaseMessage::human(deterministicText(256 * 1024, 3));
    largeMessage.id_ = "large-message";
    auto decodedMessage = lgc::baseMessageFromJson(lgc::baseMessageToJson(largeMessage));
    assert(decodedMessage.isOk());
    assert(decodedMessage->content_.size() == largeMessage.content_.size());

    auto messageState = stateFromJsonValue({
        { "messages", lgc::messagesToJson({ largeMessage }) },
    });
    const lgc::JsonStateCodec stateCodec;
    auto encodedState = stateCodec.encode(messageState);
    assert(encodedState.isOk());
    auto decodedState = stateCodec.decode(*encodedState);
    assert(decodedState.isOk());
    assert(decodedState->view().at("messages").front().at("content").get<std::string>().size()
        == largeMessage.content_.size());

    constexpr std::uint64_t kPendingWrites = 1200;
    auto checkpoint = checkpointFor("large-pending", "root", "cp-large", 1);
    checkpoint.pendingWrites_.reserve(kPendingWrites);
    for (std::uint64_t i = 0; i < kPendingWrites; ++i)
        checkpoint.pendingWrites_.push_back(checkpointWrite(i, "pending", { { "pending_index", i } }));

    const lgc::JsonCheckpointCodec checkpointCodec;
    auto encodedCheckpoint = checkpointCodec.encode(checkpoint);
    assert(encodedCheckpoint.isOk());
    auto decodedCheckpoint = checkpointCodec.decode(*encodedCheckpoint);
    assert(decodedCheckpoint.isOk());
    assert(decodedCheckpoint->pendingWrites_.size() == kPendingWrites);

    lgc::InMemorySaver saver;
    auto seed = checkpointFor("large-pending", "root", "cp-store", 2);
    assert(saver.put(seed).isOk());
    std::vector<lgc::CheckpointWrite> writes;
    writes.reserve(kPendingWrites);
    for (std::uint64_t i = 0; i < kPendingWrites; ++i)
        writes.push_back(checkpointWrite(i, "pending", { { "pending_index", i } }));
    assert(saver.putWrites(lgc::CheckpointWriteSet {
        .threadId_ = "large-pending",
        .checkpointNamespace_ = "root",
        .checkpointId_ = "cp-store",
        .taskId_ = "bulk-task",
        .taskPath_ = "bulk/path",
        .writes_ = std::move(writes),
    }).isOk());
    auto stored = saver.getTuple(lgc::CheckpointQuery::at("large-pending", "cp-store", "root"));
    assert(stored.isOk());
    assert(stored->has_value());
    assert((*stored)->pendingWrites_.size() == kPendingWrites);
    assert((*stored)->checkpoint_.pendingWrites_.size() == kPendingWrites);
}

void stressCancellationTimeoutShutdownRace()
{
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;

        lgc::StateGraph graph;
        assert(graph.addNode("slow", [&](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
            {
                std::lock_guard lock(mutex);
                started = true;
            }
            cv.notify_all();
            std::unique_lock lock(mutex);
            while (!context.cancellationToken().cancelled()) {
                if (!cv.wait_for(lock, std::chrono::milliseconds(10), [&] {
                        return context.cancellationToken().cancelled();
                    })) {
                    continue;
                }
            }
            return context.cancellationToken().check("cancelled by pressure test");
        }).isOk());
        assert(graph.addEdge(std::string(lgc::START), "slow").isOk());
        assert(graph.addEdge("slow", std::string(lgc::END)).isOk());
        auto compiled = graph.compile();
        assert(compiled.isOk());

        lgc::CancellationSource source;
        lgc::RunOptions options;
        options.cancellationToken_ = source.token();
        auto future = std::async(std::launch::async, [&] {
            return compiled->invoke(stateFromJson("{}"), options);
        });

        {
            std::unique_lock lock(mutex);
            assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
            assert(source.cancel("cancelled by pressure test"));
        }
        cv.notify_all();
        auto result = future.get();
        assert(result.isOk());
        assert(result->status_ == lgc::RunStatus::Cancelled);
    }

    {
        lgc::NodeOptions timeoutOptions;
        timeoutOptions.timeout_ = std::chrono::milliseconds(1);
        timeoutOptions.errorHandler_ = [](const lgc::Status& status, const lgc::State&, lgc::Runtime&) -> lgc::Result<lgc::NodeOutput> {
            assert(status.code() == lgc::StatusCode::DeadlineExceeded);
            return lgc::NodeOutput::update(updateFromJsonValue({ { "timed_out", true } }));
        };

        lgc::StateGraph graph;
        assert(graph.addNode("timeout", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            while (std::chrono::steady_clock::now() < deadline) {
                if (auto status = context.cancellationToken().check(); !status.isOk())
                    return status;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return lgc::StateUpdate::fromJsonValue({ { "late", true } });
        },
                   timeoutOptions)
                   .isOk());
        assert(graph.addEdge(std::string(lgc::START), "timeout").isOk());
        assert(graph.addEdge("timeout", std::string(lgc::END)).isOk());
        auto compiled = graph.compile();
        assert(compiled.isOk());
        auto result = compiled->invoke(stateFromJson("{}"));
        assert(result.isOk());
        assert(result->state_.view().at("timed_out") == true);
        assert(!result->state_.view().contains("late"));
    }

    {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool release = false;

        lgc::StateGraph graph;
        assert(graph.addNode("streaming", [&](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
            {
                std::lock_guard lock(mutex);
                started = true;
            }
            cv.notify_all();
            for (int i = 0; i < 64; ++i) {
                auto status = context.streamWriter().write("tick", { { "i", i } });
                if (!status.isOk())
                    return status;
            }
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return release; }))
                return lgc::Status::deadlineExceeded("stream shutdown release timed out");
            return lgc::StateUpdate::fromJsonValue({ { "released", true } });
        }).isOk());
        assert(graph.addEdge(std::string(lgc::START), "streaming").isOk());
        assert(graph.addEdge("streaming", std::string(lgc::END)).isOk());
        auto compiled = graph.compile();
        assert(compiled.isOk());

        auto streamResult = compiled->streamEvents(
            stateFromJson("{}"),
            {},
            lgc::RunStreamOptions {
                .capacity_ = 256,
            });
        assert(streamResult.isOk());
        auto stream = std::move(*streamResult);

        {
            std::unique_lock lock(mutex);
            assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
            release = true;
        }
        cv.notify_all();

        auto closeFuture = std::async(std::launch::async, [&stream] {
            stream.close();
        });
        assert(closeFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        closeFuture.get();

        auto resultFuture = std::async(std::launch::async, [&stream] {
            return stream.result();
        });
        assert(resultFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        (void)resultFuture.get();
    }
}

} // namespace

int main(int argc, char** argv)
{
#if LANGGRAPH_CPP_WITH_SQLITE
    if (argc == 5 && std::string_view(argv[1]) == "--sqlite-contention-child") {
        return sqliteContentionChild(
            argv[2],
            std::stoi(argv[3]),
            static_cast<std::uint64_t>(std::stoull(argv[4])));
    }
#endif

    assert(argc >= 1);
    fuzzJsonSchemaParserValidator();
    fuzzStateUpdateReducerMerge();
    fuzzCheckpointCodec();
    fuzzContentEnvelope();
    fuzzSseParser();
    fuzzMessageToolCallJsonParser();
    fuzzGraphNamespaceParser();
    fuzzRunnableConfigMergePatch();

    stressCheckpointHistory1000();
    stressConcurrentGraphInvocations1000();
    stressMultithreadStreamConsumerResume();
    stressSQLiteMultiProcessContention(argv[0]);
    stressLargeStateMessagePendingWrites();
    stressCancellationTimeoutShutdownRace();

    return 0;
}
