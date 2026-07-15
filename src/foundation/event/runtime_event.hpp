#pragma once

#include "foundation/status/status.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lgc {

struct RuntimeEventLimits {
    std::size_t maxPayloadBytes_ { 64 * 1024 };
    std::size_t maxStringLength_ { 4096 };
    std::size_t maxJsonDepth_ { 32 };
    std::size_t maxJsonNodes_ { 4096 };
    std::size_t maxJsonItems_ { 1024 };
};

struct RuntimeEventOptions {
    RuntimeEventLimits limits_;
    bool generateEventId_ { true };
    bool generateSequence_ { true };
    std::string eventIdPrefix_ { "evt" };
};

enum class RuntimeEventType : std::uint8_t {
    Unknown = 0,
    RunStarted,
    RunCompleted,
    RunFailed,
    NodeStarted,
    NodeCompleted,
    NodeFailed,
    ToolCallStarted,
    ToolCallCompleted,
    ToolCallFailed,
    Token,
    StateUpdated,
    CheckpointCreated,
    InterruptRequested,
    Custom,
};

struct RuntimeEvent {
    RuntimeEventType type_ { RuntimeEventType::Unknown };
    std::string eventId_;
    std::string runId_;
    std::string threadId_;
    std::uint64_t step_ { 0 };
    std::uint64_t sequence_ { 0 };
    std::string node_;
    std::string name_;
    std::string message_;
    nlohmann::json payload_ { nlohmann::json::object() };
    std::chrono::system_clock::time_point timestamp_ { std::chrono::system_clock::now() };

    [[nodiscard]] static RuntimeEvent create(
        RuntimeEventType type,
        const RuntimeEventOptions& options = {});
};

[[nodiscard]] std::string_view runtimeEventTypeName(RuntimeEventType type) noexcept;
[[nodiscard]] Status ensureRuntimeEventIdentity(
    RuntimeEvent& event,
    const RuntimeEventOptions& options = {});
[[nodiscard]] Status validateRuntimeEvent(
    const RuntimeEvent& event,
    const RuntimeEventLimits& limits = {});

} // namespace lgc
