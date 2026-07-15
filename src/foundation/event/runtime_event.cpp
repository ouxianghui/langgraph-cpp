#include "foundation/event/runtime_event.hpp"

#include "foundation/id/id_generator.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] IdGenerator& runtimeEventIdGenerator()
{
    static IdGenerator generator = IdGenerator::ulid("evt");
    return generator;
}

[[nodiscard]] std::uint64_t nextRuntimeEventSequence() noexcept
{
    static std::atomic<std::uint64_t> sequence { 1 };
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] bool validRuntimeIdChar(unsigned char ch) noexcept
{
    return std::isalnum(ch) != 0
        || ch == '_'
        || ch == '-'
        || ch == '.'
        || ch == ':'
        || ch == '/'
        || ch == '@';
}

[[nodiscard]] Status validateStringLength(std::string_view value, std::string_view field, const RuntimeEventLimits& limits)
{
    if (limits.maxStringLength_ == 0U || value.size() <= limits.maxStringLength_)
        return Status::ok();

    std::string message("runtime event field is too long: ");
    message.append(field);
    return Status::resourceExhausted(std::move(message));
}

[[nodiscard]] Status validateIdField(std::string_view value, std::string_view field, const RuntimeEventLimits& limits)
{
    if (value.empty())
        return Status::ok();
    if (auto status = validateStringLength(value, field, limits); !status.isOk())
        return status;

    for (const auto ch : value) {
        if (!validRuntimeIdChar(static_cast<unsigned char>(ch))) {
            std::string message("runtime event id field contains invalid characters: ");
            message.append(field);
            return Status::invalidArgument(std::move(message));
        }
    }
    return Status::ok();
}

[[nodiscard]] Status validateJsonValue(
    const nlohmann::json& value,
    std::size_t depth,
    std::size_t& nodes,
    const RuntimeEventLimits& limits)
{
    if (limits.maxJsonDepth_ != 0U && depth > limits.maxJsonDepth_)
        return Status::resourceExhausted("runtime event payload exceeds max json depth");
    if (limits.maxJsonNodes_ != 0U && ++nodes > limits.maxJsonNodes_)
        return Status::resourceExhausted("runtime event payload exceeds max json nodes");

    if (value.is_object()) {
        if (limits.maxJsonItems_ != 0U && value.size() > limits.maxJsonItems_)
            return Status::resourceExhausted("runtime event payload object has too many fields");
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (auto status = validateStringLength(it.key(), "payload_key", limits); !status.isOk())
                return status;
            if (auto status = validateJsonValue(*it, depth + 1, nodes, limits); !status.isOk())
                return status;
        }
    } else if (value.is_array()) {
        if (limits.maxJsonItems_ != 0U && value.size() > limits.maxJsonItems_)
            return Status::resourceExhausted("runtime event payload array has too many items");
        for (const auto& item : value) {
            if (auto status = validateJsonValue(item, depth + 1, nodes, limits); !status.isOk())
                return status;
        }
    } else if (value.is_string()) {
        const auto text = value.get_ref<const std::string&>();
        if (auto status = validateStringLength(text, "payload_string", limits); !status.isOk())
            return status;
    }

    return Status::ok();
}

[[nodiscard]] Status validatePayload(const nlohmann::json& payload, const RuntimeEventLimits& limits)
{
    if (payload.is_discarded())
        return Status::invalidArgument("runtime event payload is invalid json");

    std::size_t nodes = 0;
    if (auto status = validateJsonValue(payload, 0, nodes, limits); !status.isOk())
        return status;

    try {
        const auto size = payload.dump().size();
        if (limits.maxPayloadBytes_ != 0U && size > limits.maxPayloadBytes_)
            return Status::resourceExhausted("runtime event payload exceeds max bytes");
    } catch (const nlohmann::json::exception& error) {
        std::string message("runtime event payload serialization failed: ");
        message.append(error.what());
        return Status::invalidArgument(std::move(message));
    }

    return Status::ok();
}

} // namespace

RuntimeEvent RuntimeEvent::create(RuntimeEventType type, const RuntimeEventOptions& options)
{
    RuntimeEvent event;
    event.type_ = type;
    event.timestamp_ = std::chrono::system_clock::now();
    (void)ensureRuntimeEventIdentity(event, options);
    return event;
}

std::string_view runtimeEventTypeName(RuntimeEventType type) noexcept
{
    switch (type) {
    case RuntimeEventType::Unknown:
        return "unknown";
    case RuntimeEventType::RunStarted:
        return "run_started";
    case RuntimeEventType::RunCompleted:
        return "run_completed";
    case RuntimeEventType::RunFailed:
        return "run_failed";
    case RuntimeEventType::NodeStarted:
        return "node_started";
    case RuntimeEventType::NodeCompleted:
        return "node_completed";
    case RuntimeEventType::NodeFailed:
        return "node_failed";
    case RuntimeEventType::ToolCallStarted:
        return "tool_call_started";
    case RuntimeEventType::ToolCallCompleted:
        return "tool_call_completed";
    case RuntimeEventType::ToolCallFailed:
        return "tool_call_failed";
    case RuntimeEventType::Token:
        return "token";
    case RuntimeEventType::StateUpdated:
        return "state_updated";
    case RuntimeEventType::CheckpointCreated:
        return "checkpoint_created";
    case RuntimeEventType::InterruptRequested:
        return "interrupt_requested";
    case RuntimeEventType::Custom:
        return "custom";
    }
    return "unknown";
}

Status ensureRuntimeEventIdentity(RuntimeEvent& event, const RuntimeEventOptions& options)
{
    if (event.eventId_.empty() && options.generateEventId_) {
        auto id = runtimeEventIdGenerator().next(options.eventIdPrefix_);
        if (!id.isOk())
            return id.status();
        event.eventId_ = std::move(*id);
    }

    if (event.sequence_ == 0U && options.generateSequence_)
        event.sequence_ = nextRuntimeEventSequence();

    if (event.timestamp_ == std::chrono::system_clock::time_point {})
        event.timestamp_ = std::chrono::system_clock::now();

    return Status::ok();
}

Status validateRuntimeEvent(const RuntimeEvent& event, const RuntimeEventLimits& limits)
{
    if (event.type_ == RuntimeEventType::Unknown)
        return Status::invalidArgument("runtime event type cannot be unknown");
    if (event.eventId_.empty())
        return Status::invalidArgument("runtime event id cannot be empty");
    if (event.sequence_ == 0U)
        return Status::invalidArgument("runtime event sequence must be greater than zero");
    if (event.type_ == RuntimeEventType::Custom && event.name_.empty())
        return Status::invalidArgument("custom runtime event name cannot be empty");

    const std::array<std::pair<std::string_view, std::string_view>, 7> strings {
        std::pair<std::string_view, std::string_view> { event.eventId_, "event_id" },
        { event.runId_, "run_id" },
        { event.threadId_, "thread_id" },
        { event.node_, "node" },
        { event.name_, "name" },
        { event.message_, "message" },
        { runtimeEventTypeName(event.type_), "type" },
    };

    for (const auto& [value, field] : strings) {
        if (auto status = validateStringLength(value, field, limits); !status.isOk())
            return status;
    }

    if (auto status = validateIdField(event.eventId_, "event_id", limits); !status.isOk())
        return status;
    if (auto status = validateIdField(event.runId_, "run_id", limits); !status.isOk())
        return status;
    if (auto status = validateIdField(event.threadId_, "thread_id", limits); !status.isOk())
        return status;

    if (auto status = validatePayload(event.payload_, limits); !status.isOk())
        return status;
    return Status::ok();
}

} // namespace lgc
