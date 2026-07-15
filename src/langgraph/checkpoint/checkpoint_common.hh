#pragma once

#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"
#include "foundation/storage/i_storage.hpp"
#include "langgraph/checkpoint/storage_saver.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lgc::detail {

[[nodiscard]] Result<void> validateCheckpointForStore(const Checkpoint& checkpoint);
[[nodiscard]] Result<void> validateCheckpointQuery(const CheckpointQuery& query);
[[nodiscard]] Result<void> validateCheckpointListOptions(const CheckpointListOptions& options);
[[nodiscard]] Result<CheckpointWriteSet> normalizeWriteSet(CheckpointWriteSet writes);
[[nodiscard]] std::string checkpointWriteStorageKey(
    std::string_view threadId,
    std::string_view checkpointNamespace,
    std::string_view checkpointId);
[[nodiscard]] bool checkpointIsNewer(const Checkpoint& lhs, const Checkpoint& rhs);
[[nodiscard]] std::string hexEncode(std::string_view value);
[[nodiscard]] Result<void> validateStorageSaverOptions(
    const std::shared_ptr<IStorage>& storage,
    const StorageSaverOptions& options);
[[nodiscard]] Result<std::string> storageValueFromPayload(const Payload& payload);
[[nodiscard]] Result<Payload> payloadFromStorageValue(std::string_view value);
[[nodiscard]] std::vector<CheckpointTuple> applyListOptions(
    std::vector<Checkpoint> checkpoints,
    const std::map<std::string, std::vector<CheckpointWrite>>& writesByCheckpoint,
    const CheckpointListOptions& options);
[[nodiscard]] std::string paddedIndex(std::uint64_t value);
[[nodiscard]] Result<void> validateDeleteForRunsOptions(
    const CheckpointDeleteForRunsOptions& options);
[[nodiscard]] bool checkpointIdMatchesRun(
    std::string_view checkpointId,
    const std::vector<std::string>& runIds);
[[nodiscard]] Result<CheckpointMaintenanceResult> copyThreadWith(
    BaseCheckpointSaver& checkpointer,
    CheckpointCopyThreadOptions options);
[[nodiscard]] Result<DeltaChannelHistories> getDeltaChannelHistoryWith(
    BaseCheckpointSaver& checkpointer,
    DeltaChannelHistoryQuery query);

} // namespace lgc::detail
