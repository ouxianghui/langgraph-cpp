#pragma once

#include "langgraph/checkpoint/saver.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lc {

/// In-memory checkpoint store for tests and single-process examples.
class InMemorySaver final : public BaseCheckpointSaver {
public:
    [[nodiscard]] Result<void> put(Checkpoint checkpoint) override;
    [[nodiscard]] Result<void> putWrites(CheckpointWriteSet writes) override;
    [[nodiscard]] Result<std::optional<Checkpoint>> get(CheckpointQuery query) override;
    [[nodiscard]] Result<std::optional<CheckpointTuple>> getTuple(CheckpointQuery query) override;
    [[nodiscard]] Result<std::vector<CheckpointTuple>> list(CheckpointListOptions options) override;
    [[nodiscard]] Result<void> deleteThread(std::string_view threadId) override;
    [[nodiscard]] Result<CheckpointMaintenanceResult> prune(
        std::string_view threadId,
        const CheckpointPruneOptions& options) override;
    [[nodiscard]] Result<CheckpointMaintenanceResult> copyThread(
        CheckpointCopyThreadOptions options) override;
    [[nodiscard]] Result<CheckpointMaintenanceResult> deleteForRuns(
        CheckpointDeleteForRunsOptions options) override;
    [[nodiscard]] Result<DeltaChannelHistories> getDeltaChannelHistory(
        DeltaChannelHistoryQuery query) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::vector<Checkpoint>> checkpoints_;
    std::map<std::string, std::vector<CheckpointWrite>> writes_;
};

} // namespace lc
