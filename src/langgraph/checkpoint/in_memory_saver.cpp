#include "langgraph/checkpoint/checkpoint_common.hh"
#include "langgraph/checkpoint/in_memory_saver.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace lc {

using namespace detail;

Result<void> InMemorySaver::put(Checkpoint checkpoint)
{
    if (auto result = validateCheckpointForStore(checkpoint); !result.isOk())
        return result.status();

    std::lock_guard lock(mutex_);
    auto& threadCheckpoints = checkpoints_[checkpoint.threadId_];
    const auto duplicate = std::find_if(
        threadCheckpoints.begin(),
        threadCheckpoints.end(),
        [&](const Checkpoint& existing) {
            return existing.checkpointNamespace_ == checkpoint.checkpointNamespace_
                && existing.checkpointId_ == checkpoint.checkpointId_;
        });
    if (duplicate != threadCheckpoints.end())
        return Status::alreadyExists("checkpoint already exists");

    threadCheckpoints.push_back(std::move(checkpoint));
    std::stable_sort(
        threadCheckpoints.begin(),
        threadCheckpoints.end(),
        [](const Checkpoint& lhs, const Checkpoint& rhs) {
            if (lhs.step_ != rhs.step_)
                return lhs.step_ < rhs.step_;
            return lhs.createdAt_ < rhs.createdAt_;
        });
    return okResult();
}

Result<void> InMemorySaver::putWrites(CheckpointWriteSet writes)
{
    auto normalized = normalizeWriteSet(std::move(writes));
    if (!normalized.isOk())
        return normalized.status();

    std::lock_guard lock(mutex_);
    auto& stored = writes_[checkpointWriteStorageKey(
        normalized->threadId_,
        normalized->checkpointNamespace_,
        normalized->checkpointId_)];
    for (auto& write : normalized->writes_) {
        const auto duplicate = std::find_if(
            stored.begin(),
            stored.end(),
            [&](const CheckpointWrite& existing) {
                return existing.taskId_ == write.taskId_
                    && existing.taskPath_ == write.taskPath_
                    && existing.order_ == write.order_;
            });
        if (duplicate == stored.end()) {
            stored.push_back(std::move(write));
        } else {
            *duplicate = std::move(write);
        }
    }
    std::stable_sort(
        stored.begin(),
        stored.end(),
        [](const CheckpointWrite& lhs, const CheckpointWrite& rhs) {
            if (lhs.taskId_ != rhs.taskId_)
                return lhs.taskId_ < rhs.taskId_;
            if (lhs.taskPath_ != rhs.taskPath_)
                return lhs.taskPath_ < rhs.taskPath_;
            return lhs.order_.value_or(std::numeric_limits<std::uint64_t>::max())
                < rhs.order_.value_or(std::numeric_limits<std::uint64_t>::max());
        });
    return okResult();
}

Result<std::optional<Checkpoint>> InMemorySaver::get(CheckpointQuery query)
{
    auto tuple = getTuple(std::move(query));
    if (!tuple.isOk())
        return tuple.status();
    if (!tuple->has_value())
        return std::optional<Checkpoint> {};
    return std::optional<Checkpoint>(std::move((*tuple)->checkpoint_));
}

Result<std::optional<CheckpointTuple>> InMemorySaver::getTuple(CheckpointQuery query)
{
    if (auto result = validateCheckpointQuery(query); !result.isOk())
        return result.status();

    std::lock_guard lock(mutex_);
    const auto found = checkpoints_.find(query.threadId_);
    if (found == checkpoints_.end() || found->second.empty())
        return std::optional<CheckpointTuple> {};

    auto checkpoint = found->second.end();
    if (query.checkpointId_.has_value()) {
        checkpoint = std::find_if(
            found->second.begin(),
            found->second.end(),
            [&](const Checkpoint& candidate) {
                return candidate.checkpointNamespace_ == query.checkpointNamespace_
                    && candidate.checkpointId_ == *query.checkpointId_;
            });
    } else {
        for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
            if (it->checkpointNamespace_ == query.checkpointNamespace_) {
                checkpoint = std::prev(it.base());
                break;
            }
        }
    }
    if (checkpoint == found->second.end())
        return std::optional<CheckpointTuple> {};

    Checkpoint copy = *checkpoint;
    auto pendingWrites = copy.pendingWrites_;
    const auto writes = writes_.find(checkpointWriteStorageKey(
        copy.threadId_,
        copy.checkpointNamespace_,
        copy.checkpointId_));
    if (pendingWrites.empty() && writes != writes_.end())
        pendingWrites = writes->second;
    if (copy.pendingWrites_.empty() && !pendingWrites.empty())
        copy.pendingWrites_ = pendingWrites;
    return std::optional<CheckpointTuple>(CheckpointTuple {
        .checkpoint_ = std::move(copy),
        .pendingWrites_ = std::move(pendingWrites),
    });
}

Result<std::vector<CheckpointTuple>> InMemorySaver::list(CheckpointListOptions options)
{
    if (auto result = validateCheckpointListOptions(options); !result.isOk())
        return result.status();

    std::lock_guard lock(mutex_);
    const auto found = checkpoints_.find(options.threadId_);
    if (found == checkpoints_.end())
        return std::vector<CheckpointTuple> {};

    std::vector<Checkpoint> checkpoints;
    checkpoints.reserve(found->second.size());
    for (const auto& checkpoint : found->second) {
        if (options.checkpointNamespace_.has_value()
            && checkpoint.checkpointNamespace_ != *options.checkpointNamespace_) {
            continue;
        }
        checkpoints.push_back(checkpoint);
    }
    return applyListOptions(std::move(checkpoints), writes_, options);
}

Result<void> InMemorySaver::deleteThread(std::string_view threadId)
{
    std::lock_guard lock(mutex_);
    const std::string thread(threadId);
    checkpoints_.erase(thread);
    for (auto it = writes_.begin(); it != writes_.end();) {
        if (it->first.starts_with(std::to_string(thread.size()) + ":" + thread + "|"))
            it = writes_.erase(it);
        else
            ++it;
    }
    return okResult();
}

Result<CheckpointMaintenanceResult> InMemorySaver::prune(
    std::string_view threadId,
    const CheckpointPruneOptions& options)
{
    std::lock_guard lock(mutex_);
    const auto found = checkpoints_.find(std::string(threadId));
    CheckpointMaintenanceResult result;
    if (found == checkpoints_.end())
        return okResult(std::move(result));

    std::vector<std::size_t> matchingIndexes;
    for (std::size_t i = 0; i < found->second.size(); ++i) {
        if (found->second[i].checkpointNamespace_ == options.checkpointNamespace_)
            matchingIndexes.push_back(i);
    }

    const auto removeCount = matchingIndexes.size() > options.keepLatest_
        ? matchingIndexes.size() - options.keepLatest_
        : 0U;
    std::set<std::string> removedCheckpointIds;
    for (std::size_t i = 0; i < removeCount; ++i)
        removedCheckpointIds.insert(found->second[matchingIndexes[i]].checkpointId_);

    auto& checkpoints = found->second;
    checkpoints.erase(
        std::remove_if(
            checkpoints.begin(),
            checkpoints.end(),
            [&](const Checkpoint& checkpoint) {
                return checkpoint.checkpointNamespace_ == options.checkpointNamespace_
                    && removedCheckpointIds.contains(checkpoint.checkpointId_);
            }),
        checkpoints.end());
    result.removed_ = removedCheckpointIds.size();

    for (const auto& checkpointId : removedCheckpointIds) {
        writes_.erase(checkpointWriteStorageKey(
            std::string(threadId),
            options.checkpointNamespace_,
            checkpointId));
    }

    for (const auto& checkpoint : checkpoints) {
        if (checkpoint.checkpointNamespace_ != options.checkpointNamespace_)
            continue;
        ++result.remaining_;
        result.latestCheckpointId_ = checkpoint.checkpointId_;
    }
    return okResult(std::move(result));
}

Result<CheckpointMaintenanceResult> InMemorySaver::copyThread(
    CheckpointCopyThreadOptions options)
{
    return copyThreadWith(*this, std::move(options));
}

Result<CheckpointMaintenanceResult> InMemorySaver::deleteForRuns(
    CheckpointDeleteForRunsOptions options)
{
    if (auto result = validateDeleteForRunsOptions(options); !result.isOk())
        return result.status();

    std::lock_guard lock(mutex_);
    auto found = checkpoints_.find(options.threadId_);
    CheckpointMaintenanceResult result;
    if (found == checkpoints_.end())
        return okResult(std::move(result));

    std::set<std::string> affectedNamespaces;
    std::vector<std::pair<std::string, std::string>> removed;
    auto& checkpoints = found->second;
    checkpoints.erase(
        std::remove_if(
            checkpoints.begin(),
            checkpoints.end(),
            [&](const Checkpoint& checkpoint) {
                if (options.checkpointNamespace_.has_value()
                    && checkpoint.checkpointNamespace_ != *options.checkpointNamespace_) {
                    return false;
                }
                if (!checkpointIdMatchesRun(checkpoint.checkpointId_, options.runIds_))
                    return false;
                affectedNamespaces.insert(checkpoint.checkpointNamespace_);
                removed.emplace_back(checkpoint.checkpointNamespace_, checkpoint.checkpointId_);
                return true;
            }),
        checkpoints.end());

    result.removed_ = removed.size();
    for (const auto& [checkpointNamespace, checkpointId] : removed) {
        writes_.erase(checkpointWriteStorageKey(
            options.threadId_,
            checkpointNamespace,
            checkpointId));
    }

    for (const auto& checkpoint : checkpoints) {
        if (!affectedNamespaces.empty() && !affectedNamespaces.contains(checkpoint.checkpointNamespace_))
            continue;
        ++result.remaining_;
        result.latestCheckpointId_ = checkpoint.checkpointId_;
    }
    return okResult(std::move(result));
}

Result<DeltaChannelHistories> InMemorySaver::getDeltaChannelHistory(
    DeltaChannelHistoryQuery query)
{
    return getDeltaChannelHistoryWith(*this, std::move(query));
}

} // namespace lc
