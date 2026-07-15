#pragma once

#include "foundation/status/result.hpp"
#include "langgraph/checkpoint/checkpoint_types.hpp"

#include <string_view>
#include <vector>

namespace lgc {

/// LangGraph-style thread-scoped checkpoint saver used by graph resume.
///
/// `threadId` is a logical LangGraph thread, not a C++ OS thread. Implementations
/// must preserve checkpoint namespace isolation and return explicit Status /
/// Result failures for storage, codec, retention, and corruption errors.
class BaseCheckpointSaver {
public:
    virtual ~BaseCheckpointSaver() = default;

    [[nodiscard]] virtual Result<void> put(Checkpoint checkpoint) = 0;
    [[nodiscard]] virtual Result<void> putWrites(CheckpointWriteSet writes) = 0;
    [[nodiscard]] virtual Result<std::optional<Checkpoint>> get(CheckpointQuery query) = 0;
    [[nodiscard]] virtual Result<std::optional<CheckpointTuple>> getTuple(CheckpointQuery query) = 0;
    [[nodiscard]] virtual Result<std::vector<CheckpointTuple>> list(CheckpointListOptions options) = 0;
    [[nodiscard]] virtual Result<void> deleteThread(std::string_view threadId) = 0;
    [[nodiscard]] virtual Result<CheckpointMaintenanceResult> prune(
        std::string_view threadId,
        const CheckpointPruneOptions& options) = 0;
    [[nodiscard]] virtual Result<CheckpointMaintenanceResult> copyThread(
        CheckpointCopyThreadOptions options) = 0;
    [[nodiscard]] virtual Result<CheckpointMaintenanceResult> deleteForRuns(
        CheckpointDeleteForRunsOptions options) = 0;
    [[nodiscard]] virtual Result<DeltaChannelHistories> getDeltaChannelHistory(
        DeltaChannelHistoryQuery query) = 0;
};

} // namespace lgc
