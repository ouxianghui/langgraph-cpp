#pragma once

#include "foundation/event/runtime_event.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "langgraph/core/ids.hpp"
#include "langgraph/graph/run_config.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lc {

struct RunStreamOptions {
    std::size_t capacity_ { 1024 };
};

enum class StreamMode {
    Events,
    Updates,
    Values,
    Messages,
    Custom,
    Checkpoints,
    Tasks,
    Debug,
    Interrupts,
    Errors,
    Output,
};

enum class StreamProtocolVersion {
    Legacy,
    V2,
};

struct StreamPart {
    StreamMode mode_ { StreamMode::Events };
    std::string ns_;
    StepId step_ { 0 };
    std::string node_;
    std::string name_;
    nlohmann::json data_ { nlohmann::json::object() };
    RuntimeEvent event_;
};

struct RunProjectionOptions {
    std::vector<StreamMode> modes_ {
        StreamMode::Updates,
        StreamMode::Values,
        StreamMode::Messages,
        StreamMode::Custom,
        StreamMode::Interrupts,
    };
    std::size_t capacity_ { 1024 };
    std::vector<std::string> outputKeys_;
    bool includeSubgraphs_ { true };
    /// When set to V2, StreamPart::data_ is emitted as
    /// {"type": <mode>, "ns": <namespace path>, "data": <payload>} and values
    /// parts lift "__interrupt__" into a top-level "interrupts" field, matching
    /// Python LangGraph stream(..., version="v2").
    StreamProtocolVersion version_ { StreamProtocolVersion::Legacy };
    /// When true, StreamMode::Events data is emitted as a LangGraph-style event envelope.
    bool langGraphProtocol_ { false };
};

struct RunEventStreamState;

class RunEventStream final {
public:
    using Duration = std::chrono::steady_clock::duration;

    RunEventStream() = default;
    ~RunEventStream();

    RunEventStream(const RunEventStream&) = delete;
    RunEventStream& operator=(const RunEventStream&) = delete;
    RunEventStream(RunEventStream&&) noexcept = default;
    RunEventStream& operator=(RunEventStream&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Result<std::optional<RuntimeEvent>> next();
    [[nodiscard]] Result<std::optional<RuntimeEvent>> nextFor(Duration timeout);
    [[nodiscard]] Result<RunResult> result();
    void close() noexcept;

private:
    friend class CompiledStateGraph;

    explicit RunEventStream(std::shared_ptr<RunEventStreamState> state);

    std::shared_ptr<RunEventStreamState> state_;
};

struct RunPartStreamState;

class RunPartStream final {
public:
    using Duration = std::chrono::steady_clock::duration;

    RunPartStream() = default;
    ~RunPartStream();

    RunPartStream(const RunPartStream&) = delete;
    RunPartStream& operator=(const RunPartStream&) = delete;
    RunPartStream(RunPartStream&&) noexcept = default;
    RunPartStream& operator=(RunPartStream&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Result<std::optional<StreamPart>> next();
    [[nodiscard]] Result<std::optional<StreamPart>> nextFor(Duration timeout);
    [[nodiscard]] Result<RunResult> result();
    void close() noexcept;

private:
    friend class CompiledStateGraph;

    explicit RunPartStream(std::shared_ptr<RunPartStreamState> state);

    std::shared_ptr<RunPartStreamState> state_;
};

[[nodiscard]] RunStatus runStatusFromStatus(const Status& status) noexcept;

} // namespace lc
