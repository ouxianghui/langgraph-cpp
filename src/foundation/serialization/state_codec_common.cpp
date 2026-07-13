#include "foundation/serialization/state_codec_common.hh"

#include <chrono>
#include <limits>
#include <string>
#include <utility>

namespace lc::state_codec_detail {

[[nodiscard]] std::int64_t toUnixMs(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::system_clock::time_point fromUnixMs(std::int64_t value)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

    [[nodiscard]] Result<json> parseJsonObject(
        std::string_view text,
        std::string_view label,
        const JsonDecodeLimits& limits)
    {
        auto parsed = parseJsonWithLimits(text, label, limits);
        if (!parsed.isOk())
            return parsed.status();
        if (!parsed->is_object()) {
            std::string message(label);
            message.append(" must be a JSON object");
            return Status::invalidArgument(std::move(message));
        }
        return parsed;
    }

[[nodiscard]] Result<void> requireJsonPayload(const Payload& payload)
{
    if (!isJsonPayload(payload.contentType_))
        return Status::invalidArgument("payload content type must be application/json");
    return okResult();
}

[[nodiscard]] Result<void> requireCheckpointPayload(const Payload& payload)
{
    if (!isCheckpointPayload(payload.contentType_))
        return Status::invalidArgument("payload content type must be checkpoint json");
    return okResult();
}

[[nodiscard]] Result<void> validateCheckpoint(const Checkpoint& checkpoint)
{
    if (checkpoint.threadId_.empty())
        return Status::invalidArgument("checkpoint thread_id cannot be empty");
    if (checkpoint.checkpointId_.empty())
        return Status::invalidArgument("checkpoint checkpoint_id cannot be empty");
    for (const auto& nextNode : checkpoint.nextNodes_) {
        if (nextNode.empty())
            return Status::invalidArgument("checkpoint next node cannot be empty");
    }
    for (const auto& task : checkpoint.nextTasks_) {
        if (task.nodeId_.empty())
            return Status::invalidArgument("checkpoint task node_id cannot be empty");
        if (!task.interrupts_.is_array())
            return Status::invalidArgument("checkpoint task interrupts must be an array");
        if (!task.metadata_.is_object())
            return Status::invalidArgument("checkpoint task metadata must be an object");
    }
    for (const auto& write : checkpoint.writes_) {
        if (write.nodeId_.empty())
            return Status::invalidArgument("checkpoint write node_id cannot be empty");
        if (!write.metadata_.is_object())
            return Status::invalidArgument("checkpoint write metadata must be an object");
        for (const auto& task : write.nextTasks_) {
            if (task.nodeId_.empty())
                return Status::invalidArgument("checkpoint write task node_id cannot be empty");
            if (!task.interrupts_.is_array())
                return Status::invalidArgument("checkpoint write task interrupts must be an array");
            if (!task.metadata_.is_object())
                return Status::invalidArgument("checkpoint write task metadata must be an object");
        }
    }
    for (const auto& write : checkpoint.pendingWrites_) {
        if (write.nodeId_.empty())
            return Status::invalidArgument("checkpoint pending write node_id cannot be empty");
        if (!write.metadata_.is_object())
            return Status::invalidArgument("checkpoint pending write metadata must be an object");
        for (const auto& task : write.nextTasks_) {
            if (task.nodeId_.empty())
                return Status::invalidArgument("checkpoint pending write task node_id cannot be empty");
            if (!task.interrupts_.is_array())
                return Status::invalidArgument("checkpoint pending write task interrupts must be an array");
            if (!task.metadata_.is_object())
                return Status::invalidArgument("checkpoint pending write task metadata must be an object");
        }
    }
    if (!checkpoint.metadata_.is_object())
        return Status::invalidArgument("checkpoint metadata must be an object");
    if (!checkpoint.channelVersions_.is_object())
        return Status::invalidArgument("checkpoint channel_versions must be an object");
    if (!checkpoint.versionsSeen_.is_object())
        return Status::invalidArgument("checkpoint versions_seen must be an object");
    return okResult();
}

[[nodiscard]] Result<void> validateCheckpointWrite(const CheckpointWrite& write, std::string_view label)
{
    if (write.nodeId_.empty()) {
        std::string message(label);
        message.append(" node_id cannot be empty");
        return Status::invalidArgument(std::move(message));
    }
    if (!write.metadata_.is_object()) {
        std::string message(label);
        message.append(" metadata must be an object");
        return Status::invalidArgument(std::move(message));
    }
    for (const auto& task : write.nextTasks_) {
        if (task.nodeId_.empty()) {
            std::string message(label);
            message.append(" task node_id cannot be empty");
            return Status::invalidArgument(std::move(message));
        }
        if (!task.interrupts_.is_array()) {
            std::string message(label);
            message.append(" task interrupts must be an array");
            return Status::invalidArgument(std::move(message));
        }
        if (!task.metadata_.is_object()) {
            std::string message(label);
            message.append(" task metadata must be an object");
            return Status::invalidArgument(std::move(message));
        }
    }
    return okResult();
}

    [[nodiscard]] Result<json> stateToJson(const State& state, const JsonDecodeLimits& limits)
    {
        // State guarantees a within-limits JSON object at construction, so embed the
        // parsed view directly instead of re-parsing its serialized form.
        (void)limits;
        return state.view();
    }

[[nodiscard]] Result<std::string> requiredString(const json& input, const char* key)
{
    if (!input.contains(key) || !input.at(key).is_string()) {
        std::string message("checkpoint field is required string: ");
        message.append(key);
        return Status::invalidArgument(std::move(message));
    }
    return input.at(key).get<std::string>();
}

[[nodiscard]] Result<State> stateFromJsonValue(const json& value, const char* key)
{
    if (!value.is_object()) {
        std::string message("checkpoint field must be JSON object: ");
        message.append(key);
        return Status::invalidArgument(std::move(message));
    }
    return State::fromJsonValue(value);
}

[[nodiscard]] Result<json> taskToJson(const CheckpointTask& task, const JsonDecodeLimits& limits)
{
    json taskJson {
        { "node_id", task.nodeId_ },
    };
    if (!task.taskId_.empty())
        taskJson["task_id"] = task.taskId_;
    if (!task.checkpointNamespace_.empty())
        taskJson["checkpoint_ns"] = task.checkpointNamespace_;
    if (task.order_.has_value())
        taskJson["order"] = *task.order_;
    if (task.error_.has_value())
        taskJson["error"] = *task.error_;
    if (!task.interrupts_.empty())
        taskJson["interrupts"] = task.interrupts_;
    if (!task.metadata_.empty())
        taskJson["metadata"] = task.metadata_;
    if (task.state_.has_value()) {
        auto taskState = stateToJson(*task.state_, limits);
        if (!taskState.isOk())
            return taskState.status();
        taskJson["state"] = std::move(*taskState);
    } else {
        taskJson["state"] = nullptr;
    }
    return taskJson;
}

[[nodiscard]] Result<CheckpointTask> taskFromJson(const json& taskValue, const JsonDecodeLimits& limits)
{
    if (!taskValue.is_object())
        return Status::invalidArgument("checkpoint task entries must be object");
    if (auto status = rejectUnknownJsonFields(
            taskValue,
            { "task_id", "node_id", "checkpoint_ns", "state", "order", "error", "interrupts", "metadata" },
            "checkpoint task",
            limits);
        !status.isOk()) {
        return status;
    }
    auto nodeId = requiredString(taskValue, "node_id");
    if (!nodeId.isOk())
        return nodeId.status();
    CheckpointTask task {
        .nodeId_ = *nodeId,
    };
    if (taskValue.contains("task_id")) {
        if (!taskValue.at("task_id").is_string())
            return Status::invalidArgument("checkpoint task task_id must be string");
        task.taskId_ = taskValue.at("task_id").get<std::string>();
    }
    if (taskValue.contains("checkpoint_ns")) {
        if (!taskValue.at("checkpoint_ns").is_string())
            return Status::invalidArgument("checkpoint task checkpoint_ns must be string");
        task.checkpointNamespace_ = taskValue.at("checkpoint_ns").get<std::string>();
    }
    if (taskValue.contains("order")) {
        if (!taskValue.at("order").is_number_unsigned())
            return Status::invalidArgument("checkpoint task order must be unsigned integer");
        task.order_ = taskValue.at("order").get<std::uint64_t>();
    }
    if (taskValue.contains("error")) {
        if (!taskValue.at("error").is_string())
            return Status::invalidArgument("checkpoint task error must be string");
        task.error_ = taskValue.at("error").get<std::string>();
    }
    if (taskValue.contains("interrupts")) {
        if (!taskValue.at("interrupts").is_array())
            return Status::invalidArgument("checkpoint task interrupts must be array");
        task.interrupts_ = taskValue.at("interrupts");
    }
    if (taskValue.contains("metadata")) {
        if (!taskValue.at("metadata").is_object())
            return Status::invalidArgument("checkpoint task metadata must be object");
        task.metadata_ = taskValue.at("metadata");
    }
    if (taskValue.contains("state") && !taskValue.at("state").is_null()) {
        auto taskState = stateFromJsonValue(taskValue.at("state"), "state");
        if (!taskState.isOk())
            return taskState.status();
        task.state_ = *taskState;
    }
    return task;
}

[[nodiscard]] Result<json> writeToJson(const CheckpointWrite& write, const JsonDecodeLimits& limits)
{
    auto update = stateToJson(write.update_, limits);
    if (!update.isOk())
        return update.status();

    json nextTasks = json::array();
    for (const auto& task : write.nextTasks_) {
        auto taskJson = taskToJson(task, limits);
        if (!taskJson.isOk())
            return taskJson.status();
        nextTasks.push_back(std::move(*taskJson));
    }

    json writeJson {
        { "node_id", write.nodeId_ },
        { "update", std::move(*update) },
        { "has_next_tasks", write.hasNextTasks_ },
    };
    if (!write.taskId_.empty())
        writeJson["task_id"] = write.taskId_;
    if (!write.taskPath_.empty())
        writeJson["task_path"] = write.taskPath_;
    if (!write.checkpointNamespace_.empty())
        writeJson["checkpoint_ns"] = write.checkpointNamespace_;
    if (write.order_.has_value())
        writeJson["order"] = *write.order_;
    if (!write.metadata_.empty())
        writeJson["metadata"] = write.metadata_;
    if (write.hasNextTasks_ || !nextTasks.empty())
        writeJson["next_tasks"] = std::move(nextTasks);
    return writeJson;
}

[[nodiscard]] Result<CheckpointWrite> writeFromJson(const json& writeValue, const JsonDecodeLimits& limits)
{
    if (!writeValue.is_object())
        return Status::invalidArgument("checkpoint writes entries must be object");
    if (auto status = rejectUnknownJsonFields(
            writeValue,
            { "task_id", "task_path", "node_id", "checkpoint_ns", "update", "order", "has_next_tasks", "next_tasks", "metadata" },
            "checkpoint write",
            limits);
        !status.isOk()) {
        return status;
    }
    auto nodeId = requiredString(writeValue, "node_id");
    if (!nodeId.isOk())
        return nodeId.status();
    if (!writeValue.contains("update"))
        return Status::invalidArgument("checkpoint write update is required");
    auto update = stateFromJsonValue(writeValue.at("update"), "update");
    if (!update.isOk())
        return update.status();

    CheckpointWrite write {
        .nodeId_ = *nodeId,
        .update_ = *update,
    };
    if (writeValue.contains("task_id")) {
        if (!writeValue.at("task_id").is_string())
            return Status::invalidArgument("checkpoint write task_id must be string");
        write.taskId_ = writeValue.at("task_id").get<std::string>();
    }
    if (writeValue.contains("task_path")) {
        if (!writeValue.at("task_path").is_string())
            return Status::invalidArgument("checkpoint write task_path must be string");
        write.taskPath_ = writeValue.at("task_path").get<std::string>();
    }
    if (writeValue.contains("checkpoint_ns")) {
        if (!writeValue.at("checkpoint_ns").is_string())
            return Status::invalidArgument("checkpoint write checkpoint_ns must be string");
        write.checkpointNamespace_ = writeValue.at("checkpoint_ns").get<std::string>();
    }
    if (writeValue.contains("order")) {
        if (!writeValue.at("order").is_number_unsigned())
            return Status::invalidArgument("checkpoint write order must be unsigned integer");
        write.order_ = writeValue.at("order").get<std::uint64_t>();
    }
    if (writeValue.contains("metadata")) {
        if (!writeValue.at("metadata").is_object())
            return Status::invalidArgument("checkpoint write metadata must be object");
        write.metadata_ = writeValue.at("metadata");
    }
    if (writeValue.contains("has_next_tasks")) {
        if (!writeValue.at("has_next_tasks").is_boolean())
            return Status::invalidArgument("checkpoint write has_next_tasks must be boolean");
        write.hasNextTasks_ = writeValue.at("has_next_tasks").get<bool>();
    }
    if (writeValue.contains("next_tasks")) {
        if (!writeValue.at("next_tasks").is_array())
            return Status::invalidArgument("checkpoint write next_tasks must be array");
        write.hasNextTasks_ = true;
        for (const auto& taskValue : writeValue.at("next_tasks")) {
            auto task = taskFromJson(taskValue, limits);
            if (!task.isOk())
                return task.status();
            write.nextTasks_.push_back(std::move(*task));
        }
    }
    return write;
}

[[nodiscard]] Result<Version> checkpointSchemaVersionFromJson(const json& input)
{
    if (!input.contains("schema_version"))
        return kCheckpointSchemaVersion;
    if (!input.at("schema_version").is_number_unsigned())
        return Status::invalidArgument("checkpoint schema_version must be unsigned integer");

    const auto value = input.at("schema_version").get<std::uint64_t>();
    if (value > std::numeric_limits<Version>::max())
        return Status::resourceExhausted("checkpoint schema_version is too large");
    return static_cast<Version>(value);
}

} // namespace lc::state_codec_detail
