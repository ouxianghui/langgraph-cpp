#include "langgraph/graph/stream_projection.hh"

#include "langgraph/graph/graph_namespace.hh"
#include "langgraph/message/message.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace lc {
namespace {

constexpr std::string_view kInterruptStateKey = "__interrupt__";

[[nodiscard]] std::string langGraphEventName(RuntimeEventType type)
{
    switch (type) {
    case RuntimeEventType::RunStarted:
        return "on_chain_start";
    case RuntimeEventType::RunCompleted:
        return "on_chain_end";
    case RuntimeEventType::RunFailed:
        return "on_chain_error";
    case RuntimeEventType::NodeStarted:
        return "on_node_start";
    case RuntimeEventType::NodeCompleted:
        return "on_node_end";
    case RuntimeEventType::NodeFailed:
        return "on_node_error";
    case RuntimeEventType::ToolCallStarted:
        return "on_tool_start";
    case RuntimeEventType::ToolCallCompleted:
        return "on_tool_end";
    case RuntimeEventType::ToolCallFailed:
        return "on_tool_error";
    case RuntimeEventType::Token:
        return "on_chat_model_stream";
    case RuntimeEventType::StateUpdated:
    case RuntimeEventType::CheckpointCreated:
    case RuntimeEventType::InterruptRequested:
    case RuntimeEventType::Custom:
        return "on_custom_event";
    case RuntimeEventType::Unknown:
        return "on_event";
    }
    return "on_event";
}

[[nodiscard]] nlohmann::json parentIdsFromEvent(const RuntimeEvent& event)
{
    nlohmann::json parentIds = nlohmann::json::array();
    if (event.payload_.is_object()
        && event.payload_.contains("parent_ids")
        && event.payload_.at("parent_ids").is_array()) {
        return event.payload_.at("parent_ids");
    }
    if (event.payload_.is_object() && event.payload_.contains("parent_run_id") && event.payload_.at("parent_run_id").is_string())
        parentIds.push_back(event.payload_.at("parent_run_id"));
    return parentIds;
}

[[nodiscard]] nlohmann::json tagsFromEvent(const RuntimeEvent& event)
{
    if (event.payload_.is_object()
        && event.payload_.contains("tags")
        && event.payload_.at("tags").is_array()) {
        return event.payload_.at("tags");
    }
    return nlohmann::json::array();
}

[[nodiscard]] std::string eventNamespace(const RuntimeEvent& event)
{
    if (event.payload_.is_object() && event.payload_.contains("ns") && event.payload_.at("ns").is_string())
        return event.payload_.at("ns").get<std::string>();
    return {};
}

[[nodiscard]] nlohmann::json namespacePathFromEvent(const RuntimeEvent& event)
{
    if (event.payload_.is_object()) {
        if (event.payload_.contains("namespace") && event.payload_.at("namespace").is_array())
            return event.payload_.at("namespace");
        if (event.payload_.contains("trace_path") && event.payload_.at("trace_path").is_array())
            return event.payload_.at("trace_path");
    }
    return detail::namespacePathFromString(eventNamespace(event));
}

[[nodiscard]] std::string streamModeProtocolName(StreamMode mode)
{
    switch (mode) {
    case StreamMode::Events:
        return "events";
    case StreamMode::Updates:
        return "updates";
    case StreamMode::Values:
        return "values";
    case StreamMode::Messages:
        return "messages";
    case StreamMode::Custom:
        return "custom";
    case StreamMode::Checkpoints:
        return "checkpoints";
    case StreamMode::Tasks:
        return "tasks";
    case StreamMode::Debug:
        return "debug";
    case StreamMode::Interrupts:
        return "interrupts";
    case StreamMode::Errors:
        return "errors";
    case StreamMode::Output:
        return "output";
    }
    return "unknown";
}

nlohmann::json popInterruptsFromValuesPayload(nlohmann::json& payload)
{
    nlohmann::json interrupts = nlohmann::json::array();
    if (!payload.is_object() || !payload.contains(kInterruptStateKey))
        return interrupts;

    const auto raw = payload.at(kInterruptStateKey);
    payload.erase(kInterruptStateKey);
    if (raw.is_array())
        return raw;
    if (raw.is_object() && raw.contains("interrupts") && raw.at("interrupts").is_array())
        return raw.at("interrupts");
    interrupts.push_back(raw);
    return interrupts;
}

void attachNamespaceFields(nlohmann::json& data, const RuntimeEvent& event)
{
    if (!data.is_object())
        return;
    const auto path = namespacePathFromEvent(event);
    if (!data.contains("ns"))
        data["ns"] = path;
    data["namespace"] = path;
    if (event.payload_.is_object() && event.payload_.contains("checkpoint_ns")) {
        data["checkpoint_ns"] = event.payload_.at("checkpoint_ns");
    } else {
        const auto ns = eventNamespace(event);
        if (!ns.empty())
            data["checkpoint_ns"] = ns;
    }
}

[[nodiscard]] nlohmann::json eventMetadata(const RuntimeEvent& event)
{
    nlohmann::json metadata = nlohmann::json::object();
    metadata["runtime_event_type"] = runtimeEventTypeName(event.type_);
    metadata["event_id"] = event.eventId_;
    metadata["thread_id"] = event.threadId_;
    metadata["run_id"] = event.runId_;
    metadata["step"] = event.step_;
    metadata["sequence"] = event.sequence_;
    metadata["namespace"] = namespacePathFromEvent(event);
    if (!event.node_.empty()) {
        metadata["node"] = event.node_;
        metadata["langgraph_node"] = event.node_;
    }
    if (event.payload_.is_object()) {
        if (event.payload_.contains("checkpoint_ns"))
            metadata["checkpoint_ns"] = event.payload_.at("checkpoint_ns");
        if (event.payload_.contains("trace_path"))
            metadata["trace_path"] = event.payload_.at("trace_path");
        if (event.payload_.contains("parent_thread_id"))
            metadata["parent_thread_id"] = event.payload_.at("parent_thread_id");
        if (event.payload_.contains("parent_node"))
            metadata["parent_node"] = event.payload_.at("parent_node");
        if (event.payload_.contains("metadata") && event.payload_.at("metadata").is_object()) {
            for (const auto& item : event.payload_.at("metadata").items())
                metadata[item.key()] = item.value();
        }
    }
    return metadata;
}

[[nodiscard]] nlohmann::json messageChunkFromTokenEvent(
    const RuntimeEvent& event,
    const nlohmann::json& metadata)
{
    const auto text = event.payload_.is_object() && event.payload_.contains("text") && event.payload_.at("text").is_string()
        ? event.payload_.at("text").get<std::string>()
        : std::string {};
    const auto contentBlocks = event.payload_.is_object()
        && event.payload_.contains("content_blocks")
        && event.payload_.at("content_blocks").is_array()
        ? event.payload_.at("content_blocks")
        : contentBlocksFromText(text);
    nlohmann::json chunk {
        { "type", "AIMessageChunk" },
        { "content", text },
        { "text", text },
        { "content_blocks", contentBlocks },
        { "metadata", metadata },
    };
    if (event.payload_.is_object()
        && event.payload_.contains("tool_call_chunks")
        && event.payload_.at("tool_call_chunks").is_array()) {
        chunk["tool_call_chunks"] = event.payload_.at("tool_call_chunks");
    }
    if (event.payload_.is_object() && event.payload_.contains("id"))
        chunk["id"] = event.payload_.at("id");
    return chunk;
}

[[nodiscard]] nlohmann::json contentBlockDeltaFromChunk(const nlohmann::json& chunk)
{
    if (!chunk.contains("content_blocks") || !chunk.at("content_blocks").is_array() || chunk.at("content_blocks").empty()) {
        return {
            { "type", "text-delta" },
            { "text", chunk.value("text", std::string {}) },
        };
    }

    const auto& block = chunk.at("content_blocks").front();
    if (!block.is_object() || !block.contains("type") || !block.at("type").is_string())
        return block;
    const auto type = block.at("type").get<std::string>();
    if (type == "text") {
        return {
            { "type", "text-delta" },
            { "text", block.value("text", std::string {}) },
        };
    }
    if (type == "reasoning") {
        return {
            { "type", "reasoning-delta" },
            { "reasoning", block.value("reasoning", std::string {}) },
        };
    }
    if (type == "tool_call_chunk") {
        nlohmann::json delta {
            { "type", "tool-call-delta" },
            { "args", block.value("args", std::string {}) },
        };
        if (block.contains("id"))
            delta["id"] = block.at("id");
        if (block.contains("name"))
            delta["name"] = block.at("name");
        if (block.contains("index"))
            delta["index"] = block.at("index");
        return delta;
    }
    return block;
}

[[nodiscard]] nlohmann::json messagesProjectionData(const RuntimeEvent& event)
{
    auto metadata = eventMetadata(event);
    auto chunk = messageChunkFromTokenEvent(event, metadata);
    const auto text = chunk.value("text", std::string {});
    nlohmann::json data {
        { "chunk", std::move(chunk) },
        { "metadata", std::move(metadata) },
        { "text", text },
        { "event", "content-block-delta" },
    };
    data["content_blocks"] = data.at("chunk").at("content_blocks");
    if (data.at("chunk").contains("tool_call_chunks"))
        data["tool_call_chunks"] = data.at("chunk").at("tool_call_chunks");
    data["delta"] = contentBlockDeltaFromChunk(data.at("chunk"));
    attachNamespaceFields(data, event);
    return data;
}

[[nodiscard]] nlohmann::json langGraphEventData(const RuntimeEvent& event)
{
    nlohmann::json data = nlohmann::json::object();
    data["payload"] = event.payload_;

    switch (event.type_) {
    case RuntimeEventType::RunStarted:
    case RuntimeEventType::NodeStarted:
    case RuntimeEventType::ToolCallStarted:
        data["input"] = event.payload_;
        break;
    case RuntimeEventType::RunCompleted:
        data["output"] = event.payload_.is_object() && event.payload_.contains("state")
            ? event.payload_.at("state")
            : event.payload_;
        break;
    case RuntimeEventType::NodeCompleted:
    case RuntimeEventType::ToolCallCompleted:
    case RuntimeEventType::StateUpdated:
        data["output"] = event.payload_;
        break;
    case RuntimeEventType::RunFailed:
    case RuntimeEventType::NodeFailed:
    case RuntimeEventType::ToolCallFailed:
        data["error"] = {
            { "message", event.message_ },
            { "payload", event.payload_ },
        };
        break;
    case RuntimeEventType::Token: {
        auto metadata = eventMetadata(event);
        data["chunk"] = messageChunkFromTokenEvent(event, metadata);
        data["metadata"] = std::move(metadata);
        break;
    }
    case RuntimeEventType::CheckpointCreated:
        data["checkpoint"] = event.payload_;
        break;
    case RuntimeEventType::InterruptRequested:
        data["interrupt"] = event.payload_;
        break;
    case RuntimeEventType::Custom:
    case RuntimeEventType::Unknown:
        break;
    }

    return data;
}

[[nodiscard]] nlohmann::json langGraphEventToJson(const RuntimeEvent& event)
{
    auto metadata = eventMetadata(event);

    return nlohmann::json {
        { "event", langGraphEventName(event.type_) },
        { "name", event.name_.empty() ? std::string(runtimeEventTypeName(event.type_)) : event.name_ },
        { "run_id", event.runId_ },
        { "parent_ids", parentIdsFromEvent(event) },
        { "tags", tagsFromEvent(event) },
        { "metadata", std::move(metadata) },
        { "data", langGraphEventData(event) },
    };
}

[[nodiscard]] nlohmann::json filterOutputKeys(
    nlohmann::json data,
    const std::vector<std::string>& outputKeys)
{
    if (outputKeys.empty() || !data.is_object())
        return data;

    nlohmann::json filtered = nlohmann::json::object();
    for (const auto& key : outputKeys) {
        if (data.contains(key))
            filtered[key] = data.at(key);
    }
    return filtered;
}

[[nodiscard]] nlohmann::json filterUpdateOutputKeys(
    nlohmann::json data,
    const std::vector<std::string>& outputKeys)
{
    if (outputKeys.empty() || !data.is_object() || !data.contains("update") || !data.at("update").is_object())
        return data;
    data["update"] = filterOutputKeys(data.at("update"), outputKeys);
    return data;
}

[[nodiscard]] nlohmann::json updateProjectionData(
    const RuntimeEvent& event,
    const RunProjectionOptions& options)
{
    auto data = filterUpdateOutputKeys(event.payload_, options.outputKeys_);
    if (data.is_object() && data.contains("update") && data.at("update").is_object()) {
        if (!event.node_.empty())
            return nlohmann::json { { event.node_, data.at("update") } };
        return data.at("update");
    }
    return data;
}

[[nodiscard]] nlohmann::json taskProjectionData(const RuntimeEvent& event)
{
    const auto taskId = event.payload_.is_object() && event.payload_.contains("task_id")
        ? event.payload_.at("task_id")
        : nlohmann::json(event.eventId_);
    nlohmann::json data {
        { "id", taskId },
        { "name", event.node_ },
    };

    switch (event.type_) {
    case RuntimeEventType::NodeStarted: {
        data["input"] = event.payload_.is_object() && event.payload_.contains("input")
            ? event.payload_.at("input")
            : event.payload_;
        data["triggers"] = event.payload_.is_object() && event.payload_.contains("triggers")
            ? event.payload_.at("triggers")
            : nlohmann::json::array();
        auto metadata = eventMetadata(event);
        if (event.payload_.is_object() && event.payload_.contains("metadata") && event.payload_.at("metadata").is_object()) {
            for (const auto& item : event.payload_.at("metadata").items())
                metadata[item.key()] = item.value();
        }
        if (!metadata.empty())
            data["metadata"] = std::move(metadata);
        break;
    }
    case RuntimeEventType::NodeCompleted:
        data["error"] = nullptr;
        data["interrupts"] = nlohmann::json::array();
        data["result"] = event.payload_.is_object() && event.payload_.contains("update")
            ? event.payload_.at("update")
            : event.payload_;
        break;
    case RuntimeEventType::NodeFailed:
        data["error"] = !event.message_.empty()
            ? nlohmann::json(event.message_)
            : event.payload_.value("error", nlohmann::json("node failed"));
        data["interrupts"] = nlohmann::json::array();
        data["result"] = nlohmann::json::object();
        break;
    default:
        data["input"] = event.payload_;
        data["triggers"] = nlohmann::json::array();
        break;
    }

    return data;
}

[[nodiscard]] std::string isoTimestamp(std::chrono::system_clock::time_point time)
{
    const auto seconds = std::chrono::system_clock::to_time_t(time);
    std::tm tm {};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count() % 1000;

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.'
        << std::setw(3)
        << std::setfill('0')
        << millis
        << 'Z';
    return out.str();
}

[[nodiscard]] nlohmann::json debugProjectionData(const RuntimeEvent& event)
{
    std::string type = "task";
    nlohmann::json payload = event.payload_;
    switch (event.type_) {
    case RuntimeEventType::CheckpointCreated:
        type = "checkpoint";
        break;
    case RuntimeEventType::NodeCompleted:
    case RuntimeEventType::NodeFailed:
        type = "task_result";
        payload = taskProjectionData(event);
        break;
    case RuntimeEventType::NodeStarted:
        type = "task";
        payload = taskProjectionData(event);
        break;
    default:
        type = "event";
        break;
    }

    return nlohmann::json {
        { "step", event.step_ },
        { "timestamp", isoTimestamp(event.timestamp_) },
        { "type", std::move(type) },
        { "payload", std::move(payload) },
    };
}

[[nodiscard]] nlohmann::json errorProjectionData(const RuntimeEvent& event)
{
    nlohmann::json data {
        { "event", runtimeEventTypeName(event.type_) },
        { "message", event.message_ },
        { "node", event.node_ },
        { "step", event.step_ },
        { "run_id", event.runId_ },
        { "thread_id", event.threadId_ },
        { "sequence", event.sequence_ },
        { "payload", event.payload_ },
        { "error", {
            { "message", event.message_ },
            { "payload", event.payload_ },
        } },
    };
    if (event.payload_.is_object()) {
        if (event.payload_.contains("task_id")) {
            data["id"] = event.payload_.at("task_id");
            data["task_id"] = event.payload_.at("task_id");
        }
        if (event.payload_.contains("checkpoint_ns"))
            data["checkpoint_ns"] = event.payload_.at("checkpoint_ns");
    }
    attachNamespaceFields(data, event);
    return data;
}

[[nodiscard]] StreamPart makeStreamPart(StreamMode mode, const RuntimeEvent& event, nlohmann::json data)
{
    return StreamPart {
        .mode_ = mode,
        .ns_ = eventNamespace(event),
        .step_ = event.step_,
        .node_ = event.node_,
        .name_ = event.name_,
        .data_ = std::move(data),
        .event_ = event,
    };
}

void applyStreamProtocolVersion(StreamPart& part, StreamProtocolVersion version)
{
    if (version != StreamProtocolVersion::V2)
        return;

    nlohmann::json payload = std::move(part.data_);
    nlohmann::json envelope {
        { "type", streamModeProtocolName(part.mode_) },
        { "ns", detail::namespacePathFromString(part.ns_) },
        { "data", std::move(payload) },
    };
    if (part.mode_ == StreamMode::Values)
        envelope["interrupts"] = popInterruptsFromValuesPayload(envelope["data"]);
    part.data_ = std::move(envelope);
}

[[nodiscard]] bool hasMode(const std::vector<StreamMode>& modes, StreamMode mode)
{
    return std::ranges::find(modes, mode) != modes.end();
}

} // namespace

namespace detail {

std::vector<StreamPart> projectEvent(
    const RuntimeEvent& event,
    const RunProjectionOptions& options,
    std::string_view rootNamespace)
{
    const auto& modes = options.modes_;
    std::vector<StreamPart> parts;
    if (hasMode(modes, StreamMode::Events))
        parts.push_back(makeStreamPart(
            StreamMode::Events,
            event,
            options.langGraphProtocol_ ? langGraphEventToJson(event) : event.payload_));
    if (hasMode(modes, StreamMode::Debug)
        && (event.type_ == RuntimeEventType::CheckpointCreated
            || event.type_ == RuntimeEventType::NodeStarted
            || event.type_ == RuntimeEventType::NodeCompleted
            || event.type_ == RuntimeEventType::NodeFailed)) {
        parts.push_back(makeStreamPart(StreamMode::Debug, event, debugProjectionData(event)));
    }
    if (hasMode(modes, StreamMode::Errors)
        && (event.type_ == RuntimeEventType::RunFailed
            || event.type_ == RuntimeEventType::NodeFailed
            || event.type_ == RuntimeEventType::ToolCallFailed)) {
        parts.push_back(makeStreamPart(StreamMode::Errors, event, errorProjectionData(event)));
    }

    switch (event.type_) {
    case RuntimeEventType::StateUpdated:
        if (hasMode(modes, StreamMode::Updates))
            parts.push_back(makeStreamPart(
                StreamMode::Updates,
                event,
                updateProjectionData(event, options)));
        break;
    case RuntimeEventType::CheckpointCreated:
        if (hasMode(modes, StreamMode::Checkpoints)) {
            auto data = event.payload_;
            parts.push_back(makeStreamPart(StreamMode::Checkpoints, event, std::move(data)));
        }
        if (hasMode(modes, StreamMode::Values) && event.payload_.contains("values"))
            parts.push_back(makeStreamPart(
                StreamMode::Values,
                event,
                filterOutputKeys(event.payload_.at("values"), options.outputKeys_)));
        break;
    case RuntimeEventType::Token:
        if (hasMode(modes, StreamMode::Messages))
            parts.push_back(makeStreamPart(StreamMode::Messages, event, messagesProjectionData(event)));
        break;
    case RuntimeEventType::Custom:
        if (hasMode(modes, StreamMode::Custom)) {
            auto data = event.payload_;
            attachNamespaceFields(data, event);
            parts.push_back(makeStreamPart(StreamMode::Custom, event, std::move(data)));
        }
        break;
    case RuntimeEventType::InterruptRequested:
        if (hasMode(modes, StreamMode::Interrupts)) {
            auto data = event.payload_;
            attachNamespaceFields(data, event);
            parts.push_back(makeStreamPart(StreamMode::Interrupts, event, std::move(data)));
        }
        break;
    case RuntimeEventType::NodeStarted:
    case RuntimeEventType::NodeCompleted:
    case RuntimeEventType::NodeFailed:
        if (hasMode(modes, StreamMode::Tasks))
            parts.push_back(makeStreamPart(StreamMode::Tasks, event, taskProjectionData(event)));
        break;
    case RuntimeEventType::RunCompleted:
        if (hasMode(modes, StreamMode::Output)) {
            auto data = event.payload_.contains("state")
                ? event.payload_.at("state")
                : event.payload_;
            parts.push_back(makeStreamPart(
                StreamMode::Output,
                event,
                filterOutputKeys(std::move(data), options.outputKeys_)));
        }
        break;
    case RuntimeEventType::Unknown:
    case RuntimeEventType::RunStarted:
    case RuntimeEventType::RunFailed:
        break;
    case RuntimeEventType::ToolCallStarted:
    case RuntimeEventType::ToolCallCompleted:
    case RuntimeEventType::ToolCallFailed:
        break;
    }

    if (!options.includeSubgraphs_) {
        std::erase_if(parts, [&](const StreamPart& part) {
            return !part.ns_.empty() && part.ns_ != rootNamespace;
        });
    }

    for (auto& part : parts)
        applyStreamProtocolVersion(part, options.version_);

    return parts;
}

} // namespace detail
} // namespace lc
