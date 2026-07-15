#pragma once

#include "foundation/storage/i_storage.hpp"
#include "langgraph/checkpoint/saver.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace lgc {

struct StorageSaverOptions {
    /// Storage scope prefix used for checkpoint keys.
    std::string scope_ { "langgraph/checkpoints" };
    std::shared_ptr<ICheckpointCodec> codec_ { std::make_shared<JsonCheckpointCodec>() };
    std::size_t listPageSize_ { 100 };
};

/// Checkpoint saver adapter backed by an IStorage implementation.
class StorageSaver final : public BaseCheckpointSaver {
public:
    explicit StorageSaver(
        std::shared_ptr<IStorage> storage,
        StorageSaverOptions options = {});

    [[nodiscard]] Result<void> put(Checkpoint checkpoint) override;
    [[nodiscard]] Result<void> putWrites(CheckpointWriteSet writes) override;
    [[nodiscard]] Result<std::optional<Checkpoint>> get(CheckpointQuery query) override;
    [[nodiscard]] Result<std::optional<CheckpointTuple>> getTuple(CheckpointQuery query) override;
    [[nodiscard]] Result<std::vector<CheckpointTuple>> list(CheckpointListOptions options) override;
    [[nodiscard]] Result<void> deleteThread(std::string_view threadId) override;
    /// Remove older checkpoints in one namespace and reconcile its latest pointer.
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
    [[nodiscard]] Result<StorageKey> keyFor(
        std::string_view threadId,
        std::string_view checkpointNamespace,
        std::string_view checkpointId) const;
    [[nodiscard]] Result<std::string> keyPrefixFor(
        std::string_view threadId,
        std::string_view checkpointNamespace) const;
    /// Key of the per-thread/per-namespace head pointer that records the latest checkpoint id.
    /// Lives outside the thread's checkpoint key prefix so list() never returns it.
    [[nodiscard]] Result<StorageKey> latestPointerKeyFor(
        std::string_view threadId,
        std::string_view checkpointNamespace) const;
    [[nodiscard]] Result<StorageKey> writeKeyFor(
        const CheckpointWriteSet& writes,
        const CheckpointWrite& write,
        std::size_t index) const;
    [[nodiscard]] Result<std::string> writeKeyPrefixFor(
        std::string_view threadId,
        std::string_view checkpointNamespace,
        std::string_view checkpointId) const;
    [[nodiscard]] Result<std::vector<CheckpointWrite>> readWrites(
        std::string_view threadId,
        std::string_view checkpointNamespace,
        std::string_view checkpointId) const;
    [[nodiscard]] Result<void> removeWrites(
        std::string_view threadId,
        std::string_view checkpointNamespace,
        std::string_view checkpointId);
    [[nodiscard]] Result<void> removeThreadWrites(std::string_view threadId);
    [[nodiscard]] Result<std::optional<Checkpoint>> getCheckpoint(
        std::string_view threadId,
        std::string_view checkpointId,
        std::string_view checkpointNamespace);
    [[nodiscard]] Result<std::vector<Checkpoint>> listCheckpoints(
        std::string_view threadId,
        std::string_view checkpointNamespace);
    [[nodiscard]] Result<std::optional<Checkpoint>> getLatestByScan(
        std::string_view threadId,
        std::string_view checkpointNamespace);
    [[nodiscard]] Result<std::optional<Checkpoint>> getLatestCheckpoint(
        std::string_view threadId,
        std::string_view checkpointNamespace);
    [[nodiscard]] Result<CheckpointMaintenanceResult> repairLatestPointer(
        std::string_view threadId,
        std::string_view checkpointNamespace = {});

    std::shared_ptr<IStorage> storage_;
    StorageSaverOptions options_;
};

} // namespace lgc
