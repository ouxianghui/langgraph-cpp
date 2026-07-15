#pragma once

#include "foundation/serialization/state_codec.hpp"
#include "foundation/versioning/versioning.hpp"

#include <chrono>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lgc::state_codec_detail {

using nlohmann::json;

inline constexpr std::string_view kJsonContentType = "application/json";
inline constexpr std::string_view kCheckpointJsonContentType = "application/vnd.langgraph-cpp.checkpoint+json";
inline constexpr std::string_view kCheckpointWriteJsonContentType = "application/vnd.langgraph-cpp.checkpoint-write+json";

[[nodiscard]] std::int64_t toUnixMs(std::chrono::system_clock::time_point value);
[[nodiscard]] std::chrono::system_clock::time_point fromUnixMs(std::int64_t value);
[[nodiscard]] Result<json> parseJsonObject(
    std::string_view text,
    std::string_view label,
    const JsonDecodeLimits& limits);
[[nodiscard]] Result<void> requireJsonPayload(const Payload& payload);
[[nodiscard]] Result<void> requireCheckpointPayload(const Payload& payload);
[[nodiscard]] Result<void> validateCheckpoint(const Checkpoint& checkpoint);
[[nodiscard]] Result<void> validateCheckpointWrite(const CheckpointWrite& write, std::string_view label);
[[nodiscard]] Result<json> stateToJson(const State& state, const JsonDecodeLimits& limits);
[[nodiscard]] Result<std::string> requiredString(const json& input, const char* key);
[[nodiscard]] Result<State> stateFromJsonValue(const json& value, const char* key);
[[nodiscard]] Result<json> taskToJson(const CheckpointTask& task, const JsonDecodeLimits& limits);
[[nodiscard]] Result<CheckpointTask> taskFromJson(const json& taskValue, const JsonDecodeLimits& limits);
[[nodiscard]] Result<json> writeToJson(const CheckpointWrite& write, const JsonDecodeLimits& limits);
[[nodiscard]] Result<CheckpointWrite> writeFromJson(const json& writeValue, const JsonDecodeLimits& limits);
[[nodiscard]] Result<Version> checkpointSchemaVersionFromJson(const json& input);

} // namespace lgc::state_codec_detail
