#include "langgraph/checkpoint/async_saver.hpp"

#include <utility>

namespace lgc {

AsyncCheckpointSaver::AsyncCheckpointSaver(
    std::shared_ptr<BaseCheckpointSaver> checkpointer,
    std::shared_ptr<IExecutor> executor)
    : checkpointer_(std::move(checkpointer))
    , executor_(std::move(executor))
{
}

Future<void> AsyncCheckpointSaver::put(Checkpoint checkpoint) const
{
    auto checkpointer = checkpointer_;
    return submitVoid([checkpointer, checkpoint = std::move(checkpoint)]() mutable {
        return checkpointer->put(std::move(checkpoint));
    });
}

Future<void> AsyncCheckpointSaver::putWrites(CheckpointWriteSet writes) const
{
    auto checkpointer = checkpointer_;
    return submitVoid([checkpointer, writes = std::move(writes)]() mutable {
        return checkpointer->putWrites(std::move(writes));
    });
}

Future<std::optional<Checkpoint>> AsyncCheckpointSaver::get(CheckpointQuery query) const
{
    auto checkpointer = checkpointer_;
    return submit<std::optional<Checkpoint>>([checkpointer, query = std::move(query)]() mutable {
        return checkpointer->get(std::move(query));
    });
}

Future<std::optional<CheckpointTuple>> AsyncCheckpointSaver::getTuple(CheckpointQuery query) const
{
    auto checkpointer = checkpointer_;
    return submit<std::optional<CheckpointTuple>>([checkpointer, query = std::move(query)]() mutable {
        return checkpointer->getTuple(std::move(query));
    });
}

Future<std::vector<CheckpointTuple>> AsyncCheckpointSaver::list(CheckpointListOptions options) const
{
    auto checkpointer = checkpointer_;
    return submit<std::vector<CheckpointTuple>>([checkpointer, options = std::move(options)]() mutable {
        return checkpointer->list(std::move(options));
    });
}

Future<void> AsyncCheckpointSaver::deleteThread(std::string threadId) const
{
    auto checkpointer = checkpointer_;
    return submitVoid([checkpointer, threadId = std::move(threadId)]() {
        return checkpointer->deleteThread(threadId);
    });
}

Future<CheckpointMaintenanceResult> AsyncCheckpointSaver::prune(
    std::string threadId,
    CheckpointPruneOptions options) const
{
    auto checkpointer = checkpointer_;
    return submit<CheckpointMaintenanceResult>(
        [checkpointer, threadId = std::move(threadId), options = std::move(options)]() {
            return checkpointer->prune(threadId, options);
        });
}

Future<CheckpointMaintenanceResult> AsyncCheckpointSaver::copyThread(
    CheckpointCopyThreadOptions options) const
{
    auto checkpointer = checkpointer_;
    return submit<CheckpointMaintenanceResult>(
        [checkpointer, options = std::move(options)]() mutable {
            return checkpointer->copyThread(std::move(options));
        });
}

Future<CheckpointMaintenanceResult> AsyncCheckpointSaver::deleteForRuns(
    CheckpointDeleteForRunsOptions options) const
{
    auto checkpointer = checkpointer_;
    return submit<CheckpointMaintenanceResult>(
        [checkpointer, options = std::move(options)]() mutable {
            return checkpointer->deleteForRuns(std::move(options));
        });
}

Future<DeltaChannelHistories> AsyncCheckpointSaver::getDeltaChannelHistory(
    DeltaChannelHistoryQuery query) const
{
    auto checkpointer = checkpointer_;
    return submit<DeltaChannelHistories>(
        [checkpointer, query = std::move(query)]() mutable {
            return checkpointer->getDeltaChannelHistory(std::move(query));
        });
}

} // namespace lgc
