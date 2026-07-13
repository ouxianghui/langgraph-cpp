#include "foundation/serialization/state_codec.hpp"

#include "foundation/serialization/state_codec_common.hh"
#include "foundation/versioning/versioning.hpp"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace lc {
namespace {
using nlohmann::json;
using namespace state_codec_detail;
}

JsonCheckpointCodec::JsonCheckpointCodec(JsonDecodeLimits limits)
    : limits_(std::move(limits))
{
}

Result<Payload> JsonCheckpointCodec::encode(const Checkpoint& checkpoint) const
{
    if (auto result = validateCheckpoint(checkpoint); !result.isOk())
        return result.status();

    auto checkpointState = stateToJson(checkpoint.state_, limits_);
    if (!checkpointState.isOk())
        return checkpointState.status();

    json writes = json::array();
    for (const auto& write : checkpoint.writes_) {
        auto writeJson = writeToJson(write, limits_);
        if (!writeJson.isOk())
            return writeJson.status();
        writes.push_back(std::move(*writeJson));
    }

    json pendingWrites = json::array();
    for (const auto& write : checkpoint.pendingWrites_) {
        auto writeJson = writeToJson(write, limits_);
        if (!writeJson.isOk())
            return writeJson.status();
        pendingWrites.push_back(std::move(*writeJson));
    }

    json nextTasks = json::array();
    for (const auto& task : checkpoint.nextTasks_) {
        auto taskJson = taskToJson(task, limits_);
        if (!taskJson.isOk())
            return taskJson.status();
        nextTasks.push_back(std::move(*taskJson));
    }

    json payload {
        { "schema_version", kCheckpointSchemaVersion },
        { "thread_id", checkpoint.threadId_ },
        { "checkpoint_id", checkpoint.checkpointId_ },
        { "checkpoint_ns", checkpoint.checkpointNamespace_ },
        { "parent_checkpoint_id", checkpoint.parentCheckpointId_.has_value()
                ? json(*checkpoint.parentCheckpointId_)
                : json(nullptr) },
        { "step", checkpoint.step_ },
        { "state", std::move(*checkpointState) },
        { "next_nodes", checkpoint.nextNodes_ },
        { "next_tasks", std::move(nextTasks) },
        { "writes", std::move(writes) },
        { "pending_writes", std::move(pendingWrites) },
        { "channel_versions", checkpoint.channelVersions_ },
        { "versions_seen", checkpoint.versionsSeen_ },
        { "updated_channels", checkpoint.updatedChannels_ },
        { "metadata", checkpoint.metadata_ },
        { "created_at_ms", toUnixMs(checkpoint.createdAt_) },
    };

    return Payload {
        .contentType_ = std::string(kCheckpointJsonContentType),
        .data_ = payload.dump(),
    };
}

Result<Checkpoint> JsonCheckpointCodec::decode(const Payload& payload) const
{
    if (auto result = requireCheckpointPayload(payload); !result.isOk())
        return result.status();

    auto parsed = parseJsonObject(payload.data_, "checkpoint", limits_);
    if (!parsed.isOk())
        return parsed.status();
    if (auto status = rejectUnknownJsonFields(
            *parsed,
            {
                "schema_version",
                "thread_id",
                "checkpoint_id",
                "checkpoint_ns",
                "parent_checkpoint_id",
                "step",
                "state",
                "next_nodes",
                "next_tasks",
                "writes",
                "pending_writes",
                "channel_versions",
                "versions_seen",
                "updated_channels",
                "metadata",
                "created_at_ms",
            },
            "checkpoint",
            limits_);
        !status.isOk()) {
        return status;
    }

    auto schemaVersion = checkpointSchemaVersionFromJson(*parsed);
    if (!schemaVersion.isOk())
        return schemaVersion.status();
    if (auto status = requireCheckpointSchemaVersion(*schemaVersion); !status.isOk())
        return status;

    auto threadId = requiredString(*parsed, "thread_id");
    if (!threadId.isOk())
        return threadId.status();
    auto checkpointId = requiredString(*parsed, "checkpoint_id");
    if (!checkpointId.isOk())
        return checkpointId.status();

    if (!parsed->contains("step") || !parsed->at("step").is_number_unsigned())
        return Status::invalidArgument("checkpoint field is required unsigned integer: step");
    if (!parsed->contains("state"))
        return Status::invalidArgument("checkpoint field is required object: state");
    auto state = stateFromJsonValue(parsed->at("state"), "state");
    if (!state.isOk())
        return state.status();

    Checkpoint checkpoint {
        .threadId_ = *threadId,
        .checkpointId_ = *checkpointId,
        .step_ = parsed->at("step").get<std::uint64_t>(),
        .state_ = *state,
    };

    if (parsed->contains("checkpoint_ns")) {
        if (!parsed->at("checkpoint_ns").is_string())
            return Status::invalidArgument("checkpoint checkpoint_ns must be string");
        checkpoint.checkpointNamespace_ = parsed->at("checkpoint_ns").get<std::string>();
    }

    if (parsed->contains("parent_checkpoint_id") && !parsed->at("parent_checkpoint_id").is_null()) {
        if (!parsed->at("parent_checkpoint_id").is_string())
            return Status::invalidArgument("checkpoint parent_checkpoint_id must be string or null");
        checkpoint.parentCheckpointId_ = parsed->at("parent_checkpoint_id").get<std::string>();
    }

    if (parsed->contains("next_nodes")) {
        if (!parsed->at("next_nodes").is_array())
            return Status::invalidArgument("checkpoint next_nodes must be array");
        for (const auto& nextNode : parsed->at("next_nodes")) {
            if (!nextNode.is_string())
                return Status::invalidArgument("checkpoint next_nodes entries must be string");
            checkpoint.nextNodes_.push_back(nextNode.get<std::string>());
        }
    }

    if (parsed->contains("next_tasks")) {
        if (!parsed->at("next_tasks").is_array())
            return Status::invalidArgument("checkpoint next_tasks must be array");
        for (const auto& taskValue : parsed->at("next_tasks")) {
            auto task = taskFromJson(taskValue, limits_);
            if (!task.isOk())
                return task.status();
            checkpoint.nextTasks_.push_back(std::move(*task));
        }
    }

    if (parsed->contains("writes")) {
        if (!parsed->at("writes").is_array())
            return Status::invalidArgument("checkpoint writes must be array");
        for (const auto& writeValue : parsed->at("writes")) {
            auto write = writeFromJson(writeValue, limits_);
            if (!write.isOk())
                return write.status();
            checkpoint.writes_.push_back(std::move(*write));
        }
    }

    if (parsed->contains("pending_writes")) {
        if (!parsed->at("pending_writes").is_array())
            return Status::invalidArgument("checkpoint pending_writes must be array");
        for (const auto& writeValue : parsed->at("pending_writes")) {
            auto write = writeFromJson(writeValue, limits_);
            if (!write.isOk())
                return write.status();
            checkpoint.pendingWrites_.push_back(std::move(*write));
        }
    }

    if (parsed->contains("created_at_ms")) {
        if (!parsed->at("created_at_ms").is_number_integer())
            return Status::invalidArgument("checkpoint created_at_ms must be integer");
        checkpoint.createdAt_ = fromUnixMs(parsed->at("created_at_ms").get<std::int64_t>());
    }

    if (parsed->contains("channel_versions")) {
        if (!parsed->at("channel_versions").is_object())
            return Status::invalidArgument("checkpoint channel_versions must be object");
        checkpoint.channelVersions_ = parsed->at("channel_versions");
    }

    if (parsed->contains("versions_seen")) {
        if (!parsed->at("versions_seen").is_object())
            return Status::invalidArgument("checkpoint versions_seen must be object");
        checkpoint.versionsSeen_ = parsed->at("versions_seen");
    }

    if (parsed->contains("updated_channels")) {
        if (!parsed->at("updated_channels").is_array())
            return Status::invalidArgument("checkpoint updated_channels must be array");
        for (const auto& channel : parsed->at("updated_channels")) {
            if (!channel.is_string())
                return Status::invalidArgument("checkpoint updated_channels entries must be string");
            checkpoint.updatedChannels_.push_back(channel.get<std::string>());
        }
    }

    if (parsed->contains("metadata")) {
        if (!parsed->at("metadata").is_object())
            return Status::invalidArgument("checkpoint metadata must be object");
        checkpoint.metadata_ = parsed->at("metadata");
    }

    if (auto result = validateCheckpoint(checkpoint); !result.isOk())
        return result.status();

    return checkpoint;
}

Result<Payload> JsonCheckpointCodec::encodeWrite(const CheckpointWrite& write) const
{
    if (auto result = validateCheckpointWrite(write, "checkpoint write"); !result.isOk())
        return result.status();

    auto writeJson = writeToJson(write, limits_);
    if (!writeJson.isOk())
        return writeJson.status();
    return Payload {
        .contentType_ = std::string(kCheckpointWriteJsonContentType),
        .data_ = writeJson->dump(),
    };
}

Result<CheckpointWrite> JsonCheckpointCodec::decodeWrite(const Payload& payload) const
{
    if (!isCheckpointWritePayload(payload.contentType_))
        return Status::invalidArgument("payload content type must be checkpoint write json");
    auto parsed = parseJsonObject(payload.data_, "checkpoint write", limits_);
    if (!parsed.isOk())
        return parsed.status();
    auto write = writeFromJson(*parsed, limits_);
    if (!write.isOk())
        return write.status();
    if (auto result = validateCheckpointWrite(*write, "checkpoint write"); !result.isOk())
        return result.status();
    return write;
}

} // namespace lc
