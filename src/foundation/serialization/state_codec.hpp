#pragma once

#include "foundation/serialization/json_limits.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

struct Payload {
    std::string contentType_;
    std::string data_;
};

class State final {
public:
    State();

    [[nodiscard]] static Result<State> fromJson(std::string jsonText);
    [[nodiscard]] static Result<State> fromJson(std::string jsonText, const JsonDecodeLimits& limits);
    [[nodiscard]] static Result<State> fromJsonValue(const nlohmann::json& value);
    [[nodiscard]] static Result<State> fromJsonValue(
        const nlohmann::json& value,
        const JsonDecodeLimits& limits);

    /// Canonical serialized form. Computed lazily and cached; cheap on repeat calls.
    [[nodiscard]] const std::string& json() const;
    /// Zero-copy, non-owning view of the parsed state. The value is guaranteed to be
    /// a JSON object within limits because every constructor validates at the boundary.
    /// Prefer this for read-only access on hot paths; it never parses or copies.
    [[nodiscard]] const nlohmann::json& view() const noexcept { return value_; }
    /// Backwards-compatible accessor. Returns a copy of the parsed state without
    /// re-parsing. Use view() when a copy is not needed.
    [[nodiscard]] Result<nlohmann::json> toJson() const;
    [[nodiscard]] Result<nlohmann::json> toJson(const JsonDecodeLimits& limits) const;

    [[nodiscard]] friend bool operator==(const State& lhs, const State& rhs) noexcept
    {
        return lhs.value_ == rhs.value_;
    }

private:
    explicit State(nlohmann::json value);

    nlohmann::json value_ { nlohmann::json::object() };
    mutable std::string serialized_;
    mutable bool serializedValid_ { false };
};

struct CheckpointTask {
    std::string taskId_;
    std::string nodeId_;
    std::string checkpointNamespace_;
    std::optional<State> state_;
    std::optional<std::uint64_t> order_;
    std::optional<std::string> error_;
    nlohmann::json interrupts_ { nlohmann::json::array() };
    nlohmann::json metadata_ { nlohmann::json::object() };

    friend bool operator==(const CheckpointTask&, const CheckpointTask&) = default;
};

struct CheckpointWrite {
    std::string taskId_;
    std::string taskPath_;
    std::string nodeId_;
    std::string checkpointNamespace_;
    State update_;
    std::optional<std::uint64_t> order_;
    bool hasNextTasks_ { false };
    std::vector<CheckpointTask> nextTasks_;
    nlohmann::json metadata_ { nlohmann::json::object() };

    friend bool operator==(const CheckpointWrite&, const CheckpointWrite&) = default;
};

struct Checkpoint {
    std::string threadId_;
    std::string checkpointId_;
    std::string checkpointNamespace_;
    std::optional<std::string> parentCheckpointId_;
    std::uint64_t step_ { 0 };
    State state_;
    std::vector<std::string> nextNodes_;
    std::vector<CheckpointTask> nextTasks_;
    std::vector<CheckpointWrite> writes_;
    std::vector<CheckpointWrite> pendingWrites_;
    nlohmann::json channelVersions_ { nlohmann::json::object() };
    nlohmann::json versionsSeen_ { nlohmann::json::object() };
    std::vector<std::string> updatedChannels_;
    nlohmann::json metadata_ { nlohmann::json::object() };
    std::chrono::system_clock::time_point createdAt_;
};

class IStateCodec {
public:
    virtual ~IStateCodec() = default;

    [[nodiscard]] virtual Result<Payload> encode(const State& state) const = 0;
    [[nodiscard]] virtual Result<State> decode(const Payload& payload) const = 0;
};

class ICheckpointCodec {
public:
    virtual ~ICheckpointCodec() = default;

    [[nodiscard]] virtual Result<Payload> encode(const Checkpoint& checkpoint) const = 0;
    [[nodiscard]] virtual Result<Checkpoint> decode(const Payload& payload) const = 0;
    [[nodiscard]] virtual Result<Payload> encodeWrite(const CheckpointWrite& write) const = 0;
    [[nodiscard]] virtual Result<CheckpointWrite> decodeWrite(const Payload& payload) const = 0;
};

class JsonStateCodec final : public IStateCodec {
public:
    explicit JsonStateCodec(JsonDecodeLimits limits = {});

    [[nodiscard]] Result<Payload> encode(const State& state) const override;
    [[nodiscard]] Result<State> decode(const Payload& payload) const override;

private:
    JsonDecodeLimits limits_;
};

class JsonCheckpointCodec final : public ICheckpointCodec {
public:
    explicit JsonCheckpointCodec(JsonDecodeLimits limits = {});

    [[nodiscard]] Result<Payload> encode(const Checkpoint& checkpoint) const override;
    [[nodiscard]] Result<Checkpoint> decode(const Payload& payload) const override;
    [[nodiscard]] Result<Payload> encodeWrite(const CheckpointWrite& write) const override;
    [[nodiscard]] Result<CheckpointWrite> decodeWrite(const Payload& payload) const override;

private:
    JsonDecodeLimits limits_;
};

[[nodiscard]] bool isJsonPayload(std::string_view contentType) noexcept;
[[nodiscard]] bool isCheckpointPayload(std::string_view contentType) noexcept;
[[nodiscard]] bool isCheckpointWritePayload(std::string_view contentType) noexcept;

} // namespace lgc
