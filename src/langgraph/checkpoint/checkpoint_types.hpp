#pragma once

#include "foundation/serialization/state_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lc {

enum class CheckpointListOrder : std::uint8_t {
    OldestFirst,
    NewestFirst,
};

struct CheckpointQuery {
    std::string threadId_;
    std::string checkpointNamespace_;
    std::optional<std::string> checkpointId_;

    [[nodiscard]] static CheckpointQuery latest(
        std::string threadId,
        std::string checkpointNamespace = {});
    [[nodiscard]] static CheckpointQuery at(
        std::string threadId,
        std::string checkpointId,
        std::string checkpointNamespace = {});
};

struct CheckpointWriteSet {
    std::string threadId_;
    std::string checkpointNamespace_;
    std::string checkpointId_;
    std::string taskId_;
    std::string taskPath_;
    std::vector<CheckpointWrite> writes_;
};

struct CheckpointTuple {
    Checkpoint checkpoint_;
    std::vector<CheckpointWrite> pendingWrites_;
};

struct CheckpointListOptions {
    std::string threadId_;
    /// Empty optional means all namespaces for the thread. A present empty string means root namespace.
    std::optional<std::string> checkpointNamespace_;
    std::optional<std::string> checkpointId_;
    std::optional<std::string> beforeCheckpointId_;
    std::optional<std::size_t> limit_;
    nlohmann::json metadataFilter_ { nlohmann::json::object() };
    CheckpointListOrder order_ { CheckpointListOrder::NewestFirst };
};

struct CheckpointPruneOptions {
    /// Namespace to prune. Empty string is the root graph namespace.
    std::string checkpointNamespace_;
    /// Number of newest checkpoints to retain. Zero removes the namespace history.
    std::size_t keepLatest_ { 0 };
};

struct CheckpointMaintenanceResult {
    std::size_t removed_ { 0 };
    std::size_t remaining_ { 0 };
    std::string latestCheckpointId_;
};

struct CheckpointCopyThreadOptions {
    std::string sourceThreadId_;
    std::string targetThreadId_;
    /// Empty optional copies all namespaces. A present string copies just that namespace.
    std::optional<std::string> checkpointNamespace_;
    /// Empty optional preserves source namespaces. A present string rewrites copied checkpoints
    /// into this namespace and is only valid when copying a single source namespace.
    std::optional<std::string> targetCheckpointNamespace_;
    /// When true, delete the target thread's checkpoint data before copying.
    bool overwriteTarget_ { false };
};

struct CheckpointDeleteForRunsOptions {
    std::string threadId_;
    /// Empty optional scans all namespaces for the thread.
    std::optional<std::string> checkpointNamespace_;
    /// Run id prefixes to remove, matching checkpoint ids such as "<run_id>-step-2".
    std::vector<std::string> runIds_;
};

struct DeltaChannelHistoryQuery {
    std::string threadId_;
    std::string checkpointNamespace_;
    std::string checkpointId_;
    std::vector<std::string> channels_;
};

struct DeltaChannelHistory {
    std::vector<CheckpointWrite> writes_;
    std::optional<nlohmann::json> seed_;
};

using DeltaChannelHistories = std::map<std::string, DeltaChannelHistory>;

} // namespace lc
