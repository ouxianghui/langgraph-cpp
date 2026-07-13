#include "langgraph/checkpoint/checkpoint_common.hh"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <set>
#include <utility>

namespace lc {
namespace detail {

[[nodiscard]] Result<void> validateCheckpointForStore(const Checkpoint& checkpoint)
{
    if (checkpoint.threadId_.empty())
        return Status::invalidArgument("checkpoint thread_id cannot be empty");
    if (checkpoint.checkpointId_.empty())
        return Status::invalidArgument("checkpoint checkpoint_id cannot be empty");
    return okResult();
}

[[nodiscard]] Result<void> validateCheckpointQuery(const CheckpointQuery& query)
{
    if (query.threadId_.empty())
        return Status::invalidArgument("checkpoint query thread_id cannot be empty");
    if (query.checkpointId_.has_value() && query.checkpointId_->empty())
        return Status::invalidArgument("checkpoint query checkpoint_id cannot be empty");
    return okResult();
}

[[nodiscard]] Result<void> validateCheckpointListOptions(const CheckpointListOptions& options)
{
    if (options.threadId_.empty())
        return Status::invalidArgument("checkpoint list thread_id cannot be empty");
    if (options.checkpointId_.has_value() && options.checkpointId_->empty())
        return Status::invalidArgument("checkpoint list checkpoint_id cannot be empty");
    if (options.beforeCheckpointId_.has_value() && options.beforeCheckpointId_->empty())
        return Status::invalidArgument("checkpoint list before checkpoint_id cannot be empty");
    if (options.limit_.has_value() && *options.limit_ == 0U)
        return Status::invalidArgument("checkpoint list limit must be greater than zero");
    if (!options.metadataFilter_.is_object())
        return Status::invalidArgument("checkpoint list metadata filter must be an object");
    return okResult();
}

[[nodiscard]] Result<CheckpointWriteSet> normalizeWriteSet(CheckpointWriteSet writes)
{
    if (writes.threadId_.empty())
        return Status::invalidArgument("checkpoint writes thread_id cannot be empty");
    if (writes.checkpointId_.empty())
        return Status::invalidArgument("checkpoint writes checkpoint_id cannot be empty");
    if (writes.taskId_.empty())
        return Status::invalidArgument("checkpoint writes task_id cannot be empty");

    for (std::size_t i = 0; i < writes.writes_.size(); ++i) {
        auto& write = writes.writes_[i];
        if (write.nodeId_.empty())
            return Status::invalidArgument("checkpoint write node_id cannot be empty");
        if (!write.metadata_.is_object())
            return Status::invalidArgument("checkpoint write metadata must be an object");
        if (write.taskId_.empty())
            write.taskId_ = writes.taskId_;
        if (write.taskPath_.empty())
            write.taskPath_ = writes.taskPath_;
        if (write.checkpointNamespace_.empty())
            write.checkpointNamespace_ = writes.checkpointNamespace_;
        if (!write.order_.has_value())
            write.order_ = static_cast<std::uint64_t>(i);
    }
    return writes;
}

[[nodiscard]] std::string checkpointWriteStorageKey(
    std::string_view threadId,
    std::string_view checkpointNamespace,
    std::string_view checkpointId)
{
    std::string out;
    out.reserve(threadId.size() + checkpointNamespace.size() + checkpointId.size() + 32U);
    out.append(std::to_string(threadId.size()));
    out.push_back(':');
    out.append(threadId);
    out.push_back('|');
    out.append(std::to_string(checkpointNamespace.size()));
    out.push_back(':');
    out.append(checkpointNamespace);
    out.push_back('|');
    out.append(std::to_string(checkpointId.size()));
    out.push_back(':');
    out.append(checkpointId);
    return out;
}

[[nodiscard]] bool metadataMatches(const nlohmann::json& metadata, const nlohmann::json& filter)
{
    if (filter.empty())
        return true;
    if (!metadata.is_object())
        return false;
    for (auto it = filter.begin(); it != filter.end(); ++it) {
        const auto found = metadata.find(it.key());
        if (found == metadata.end() || *found != it.value())
            return false;
    }
    return true;
}

[[nodiscard]] bool checkpointIsOlder(const Checkpoint& lhs, const Checkpoint& rhs)
{
    if (lhs.step_ != rhs.step_)
        return lhs.step_ < rhs.step_;
    if (lhs.createdAt_ != rhs.createdAt_)
        return lhs.createdAt_ < rhs.createdAt_;
    return lhs.checkpointId_ < rhs.checkpointId_;
}

[[nodiscard]] std::string hexEncode(std::string_view value)
{
    constexpr std::array<char, 16> digits {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string out;
    out.reserve(value.size() * 2);
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

[[nodiscard]] Result<void> validateStorageSaverOptions(
    const std::shared_ptr<IStorage>& storage,
    const StorageSaverOptions& options)
{
    if (!storage)
        return Status::invalidArgument("storage checkpointer storage cannot be null");
    if (!options.codec_)
        return Status::invalidArgument("storage checkpointer codec cannot be null");
    if (options.scope_.empty())
        return Status::invalidArgument("storage checkpointer scope cannot be empty");
    if (options.listPageSize_ == 0U)
        return Status::invalidArgument("storage checkpointer list page size must be greater than zero");
    return okResult();
}

[[nodiscard]] Result<std::string> storageValueFromPayload(const Payload& payload)
{
    if (payload.contentType_.empty())
        return Status::invalidArgument("checkpoint payload content type cannot be empty");
    return nlohmann::json {
        { "content_type", payload.contentType_ },
        { "data", payload.data_ },
    }.dump();
}

[[nodiscard]] Result<Payload> payloadFromStorageValue(std::string_view value)
{
    auto parsed = parseJsonWithLimits(value, "stored checkpoint");
    if (!parsed.isOk())
        return parsed.status();
    if (!parsed->is_object())
        return Status::invalidArgument("stored checkpoint must be a JSON object");
    if (!parsed->contains("content_type") || !parsed->at("content_type").is_string())
        return Status::invalidArgument("stored checkpoint content_type is required");
    if (!parsed->contains("data") || !parsed->at("data").is_string())
        return Status::invalidArgument("stored checkpoint data is required");
    return Payload {
        .contentType_ = parsed->at("content_type").get<std::string>(),
        .data_ = parsed->at("data").get<std::string>(),
    };
}

[[nodiscard]] bool checkpointIsNewer(const Checkpoint& lhs, const Checkpoint& rhs)
{
    return checkpointIsOlder(rhs, lhs);
}

[[nodiscard]] std::vector<CheckpointTuple> applyListOptions(
    std::vector<Checkpoint> checkpoints,
    const std::map<std::string, std::vector<CheckpointWrite>>& writesByCheckpoint,
    const CheckpointListOptions& options)
{
    std::stable_sort(checkpoints.begin(), checkpoints.end(), checkpointIsOlder);

    if (options.beforeCheckpointId_.has_value()) {
        const auto before = std::find_if(
            checkpoints.begin(),
            checkpoints.end(),
            [&](const Checkpoint& checkpoint) {
                return checkpoint.checkpointId_ == *options.beforeCheckpointId_;
            });
        if (before == checkpoints.end()) {
            checkpoints.clear();
        } else {
            checkpoints.erase(before, checkpoints.end());
        }
    }

    std::vector<CheckpointTuple> records;
    records.reserve(checkpoints.size());
    for (auto& checkpoint : checkpoints) {
        if (options.checkpointId_.has_value() && checkpoint.checkpointId_ != *options.checkpointId_)
            continue;
        if (!metadataMatches(checkpoint.metadata_, options.metadataFilter_))
            continue;

        auto key = checkpointWriteStorageKey(
            checkpoint.threadId_,
            checkpoint.checkpointNamespace_,
            checkpoint.checkpointId_);
        std::vector<CheckpointWrite> pendingWrites;
        const auto found = writesByCheckpoint.find(key);
        if (found != writesByCheckpoint.end())
            pendingWrites = found->second;
        if (pendingWrites.empty())
            pendingWrites = checkpoint.pendingWrites_;
        else if (checkpoint.pendingWrites_.empty())
            checkpoint.pendingWrites_ = pendingWrites;
        records.push_back(CheckpointTuple {
            .checkpoint_ = std::move(checkpoint),
            .pendingWrites_ = std::move(pendingWrites),
        });
    }

    if (options.order_ == CheckpointListOrder::NewestFirst)
        std::reverse(records.begin(), records.end());
    if (options.limit_.has_value() && records.size() > *options.limit_)
        records.resize(*options.limit_);
    return records;
}

[[nodiscard]] std::string paddedIndex(std::uint64_t value)
{
    auto text = std::to_string(value);
    if (text.size() >= 20U)
        return text;
    return std::string(20U - text.size(), '0') + text;
}

[[nodiscard]] Result<void> validateCopyOptions(const CheckpointCopyThreadOptions& options)
{
    if (options.sourceThreadId_.empty())
        return Status::invalidArgument("checkpoint copy source thread_id cannot be empty");
    if (options.targetThreadId_.empty())
        return Status::invalidArgument("checkpoint copy target thread_id cannot be empty");
    if (options.targetCheckpointNamespace_.has_value() && !options.checkpointNamespace_.has_value())
        return Status::invalidArgument("checkpoint copy target namespace requires a single source namespace");
    return okResult();
}

[[nodiscard]] Result<void> validateDeleteForRunsOptions(const CheckpointDeleteForRunsOptions& options)
{
    if (options.threadId_.empty())
        return Status::invalidArgument("checkpoint erase runs thread_id cannot be empty");
    if (options.runIds_.empty())
        return Status::invalidArgument("checkpoint erase runs requires at least one run id");
    for (const auto& runId : options.runIds_) {
        if (runId.empty())
            return Status::invalidArgument("checkpoint erase run id cannot be empty");
    }
    return okResult();
}

[[nodiscard]] Result<void> validateDeltaQuery(const DeltaChannelHistoryQuery& query)
{
    if (query.threadId_.empty())
        return Status::invalidArgument("checkpoint delta thread_id cannot be empty");
    if (query.checkpointId_.empty())
        return Status::invalidArgument("checkpoint delta checkpoint_id cannot be empty");
    if (query.channels_.empty())
        return Status::invalidArgument("checkpoint delta requires at least one channel");
    for (const auto& channel : query.channels_) {
        if (channel.empty())
            return Status::invalidArgument("checkpoint delta channel cannot be empty");
    }
    return okResult();
}

[[nodiscard]] bool checkpointIdMatchesRun(
    std::string_view checkpointId,
    const std::vector<std::string>& runIds)
{
    for (const auto& runId : runIds) {
        if (checkpointId == runId || checkpointId.starts_with(runId + "-"))
            return true;
    }
    return false;
}

void rewriteTaskNamespace(CheckpointTask& task, const std::optional<std::string>& targetNamespace)
{
    if (!targetNamespace.has_value())
        return;
    task.checkpointNamespace_ = *targetNamespace;
}

void rewriteWriteNamespace(CheckpointWrite& write, const std::optional<std::string>& targetNamespace)
{
    if (!targetNamespace.has_value())
        return;
    write.checkpointNamespace_ = *targetNamespace;
    for (auto& task : write.nextTasks_)
        rewriteTaskNamespace(task, targetNamespace);
}

void rewriteCheckpointForCopy(
    Checkpoint& checkpoint,
    std::string targetThreadId,
    const std::optional<std::string>& targetNamespace)
{
    checkpoint.threadId_ = std::move(targetThreadId);
    if (targetNamespace.has_value())
        checkpoint.checkpointNamespace_ = *targetNamespace;
    for (auto& task : checkpoint.nextTasks_)
        rewriteTaskNamespace(task, targetNamespace);
    for (auto& write : checkpoint.writes_)
        rewriteWriteNamespace(write, targetNamespace);
    for (auto& write : checkpoint.pendingWrites_)
        rewriteWriteNamespace(write, targetNamespace);
}

} // namespace detail

CheckpointQuery CheckpointQuery::latest(std::string threadId, std::string checkpointNamespace)
{
    return CheckpointQuery {
        .threadId_ = std::move(threadId),
        .checkpointNamespace_ = std::move(checkpointNamespace),
    };
}

CheckpointQuery CheckpointQuery::at(
    std::string threadId,
    std::string checkpointId,
    std::string checkpointNamespace)
{
    return CheckpointQuery {
        .threadId_ = std::move(threadId),
        .checkpointNamespace_ = std::move(checkpointNamespace),
        .checkpointId_ = std::move(checkpointId),
    };
}

namespace detail {

Result<CheckpointMaintenanceResult> copyThreadWith(
    BaseCheckpointSaver& checkpointer,
    CheckpointCopyThreadOptions options)
{
    if (auto result = validateCopyOptions(options); !result.isOk())
        return result.status();

    if (options.overwriteTarget_) {
        if (auto cleared = checkpointer.deleteThread(options.targetThreadId_); !cleared.isOk())
            return cleared.status();
    }

    auto records = checkpointer.list(CheckpointListOptions {
        .threadId_ = options.sourceThreadId_,
        .checkpointNamespace_ = options.checkpointNamespace_,
        .order_ = CheckpointListOrder::OldestFirst,
    });
    if (!records.isOk())
        return records.status();

    CheckpointMaintenanceResult result;
    for (auto& record : *records) {
        Checkpoint checkpoint = record.checkpoint_;
        const auto checkpointNamespace = options.targetCheckpointNamespace_.value_or(checkpoint.checkpointNamespace_);
        rewriteCheckpointForCopy(
            checkpoint,
            options.targetThreadId_,
            options.targetCheckpointNamespace_);
        if (record.pendingWrites_.empty()) {
            record.pendingWrites_ = checkpoint.pendingWrites_;
        } else if (checkpoint.pendingWrites_.empty()) {
            checkpoint.pendingWrites_ = record.pendingWrites_;
        }

        if (auto stored = checkpointer.put(checkpoint); !stored.isOk())
            return stored.status();

        if (!record.pendingWrites_.empty()) {
            auto writes = record.pendingWrites_;
            for (auto& write : writes)
                rewriteWriteNamespace(write, options.targetCheckpointNamespace_);
            if (auto storedWrites = checkpointer.putWrites(CheckpointWriteSet {
                    .threadId_ = options.targetThreadId_,
                    .checkpointNamespace_ = checkpointNamespace,
                    .checkpointId_ = checkpoint.checkpointId_,
                    .taskId_ = checkpoint.checkpointId_,
                    .taskPath_ = "copy",
                    .writes_ = std::move(writes),
                });
                !storedWrites.isOk()) {
                return storedWrites.status();
            }
        }
        ++result.remaining_;
        result.latestCheckpointId_ = checkpoint.checkpointId_;
    }
    return okResult(std::move(result));
}

Result<DeltaChannelHistories> getDeltaChannelHistoryWith(
    BaseCheckpointSaver& checkpointer,
    DeltaChannelHistoryQuery query)
{
    if (auto result = validateDeltaQuery(query); !result.isOk())
        return result.status();

    DeltaChannelHistories histories;
    std::set<std::string> remaining;
    for (const auto& channel : query.channels_) {
        histories.emplace(channel, DeltaChannelHistory {});
        remaining.insert(channel);
    }

    auto current = checkpointer.getTuple(CheckpointQuery::at(
        query.threadId_,
        query.checkpointId_,
        query.checkpointNamespace_));
    if (!current.isOk())
        return current.status();
    if (!current->has_value())
        return Status::notFound("checkpoint delta target checkpoint not found");

    auto cursor = (*current)->checkpoint_.parentCheckpointId_;
    while (cursor.has_value() && !remaining.empty()) {
        auto record = checkpointer.getTuple(CheckpointQuery::at(
            query.threadId_,
            *cursor,
            query.checkpointNamespace_));
        if (!record.isOk())
            return record.status();
        if (!record->has_value())
            break;

        std::vector<CheckpointWrite> writes = (*record)->pendingWrites_;
        if (writes.empty())
            writes = (*record)->checkpoint_.writes_;
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            for (const auto& channel : query.channels_) {
                if (!it->update_.view().contains(channel))
                    continue;
                auto copy = *it;
                histories[channel].writes_.push_back(std::move(copy));
            }
        }

        for (auto it = remaining.begin(); it != remaining.end();) {
            if ((*record)->checkpoint_.state_.view().contains(*it)) {
                histories[*it].seed_ = (*record)->checkpoint_.state_.view().at(*it);
                it = remaining.erase(it);
            } else {
                ++it;
            }
        }

        cursor = (*record)->checkpoint_.parentCheckpointId_;
    }

    for (auto& [channel, history] : histories) {
        (void)channel;
        std::reverse(history.writes_.begin(), history.writes_.end());
    }
    return okResult(std::move(histories));
}


} // namespace detail

} // namespace lc
