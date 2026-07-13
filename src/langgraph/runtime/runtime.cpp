#include "langgraph/runtime/runtime.hpp"

#include <stdexcept>
#include <utility>

namespace lc {

void RunControl::requestDrain(std::string reason)
{
    std::lock_guard lock(mutex_);
    drainReason_ = std::move(reason);
}

bool RunControl::drainRequested() const
{
    std::lock_guard lock(mutex_);
    return drainReason_.has_value();
}

std::optional<std::string> RunControl::drainReason() const
{
    std::lock_guard lock(mutex_);
    if (!drainReason_.has_value())
        return std::nullopt;
    return *drainReason_;
}

StreamWriter::StreamWriter(Options options)
    : runId_(std::move(options.runId_))
    , threadId_(std::move(options.threadId_))
    , checkpointNamespace_(std::move(options.checkpointNamespace_))
    , step_(options.step_)
    , nodeId_(std::move(options.nodeId_))
    , publisher_(std::move(options.publisher_))
{
}

Status StreamWriter::write(
    std::string name,
    nlohmann::json payload,
    std::string message) const
{
    auto event = RuntimeEvent::create(RuntimeEventType::Custom);
    event.name_ = std::move(name);
    event.payload_ = std::move(payload);
    event.message_ = std::move(message);
    return publish(std::move(event));
}

Status StreamWriter::publish(RuntimeEvent event) const
{
    if (event.runId_.empty())
        event.runId_ = runId_;
    if (event.threadId_.empty())
        event.threadId_ = threadId_;
    if (event.step_ == 0U)
        event.step_ = step_;
    if (event.node_.empty())
        event.node_ = nodeId_;
    if (!checkpointNamespace_.empty() && event.payload_.is_object()) {
        if (!event.payload_.contains("ns"))
            event.payload_["ns"] = checkpointNamespace_;
        if (!event.payload_.contains("checkpoint_ns"))
            event.payload_["checkpoint_ns"] = checkpointNamespace_;
    }
    if (publisher_)
        return publisher_(std::move(event));
    return Status::ok();
}

Runtime::Runtime(Options options)
    : context_(std::move(options.context_))
    , previous_(std::move(options.previous_))
    , executionInfo_(ExecutionInfo {
          .checkpointId_ = std::move(options.checkpointId_),
          .checkpointNamespace_ = std::move(options.checkpointNamespace_),
          .taskId_ = std::move(options.taskId_),
          .threadId_ = std::move(options.threadId_),
          .runId_ = std::move(options.runId_),
          .nodeId_ = std::move(options.nodeId_),
          .step_ = options.step_,
          .nodeAttempt_ = options.attempt_ == 0U ? 1U : options.attempt_,
          .nodeFirstAttemptTime_ = std::move(options.firstAttemptTime_),
      })
    , cancellationToken_(std::move(options.cancellationToken_))
    , streamWriter_(StreamWriter::Options {
          .runId_ = executionInfo_.runId_,
          .threadId_ = executionInfo_.threadId_,
          .checkpointNamespace_ = executionInfo_.checkpointNamespace_,
          .step_ = executionInfo_.step_,
          .nodeId_ = executionInfo_.nodeId_,
          .publisher_ = std::move(options.publisher_),
      })
    , resumeValue_(std::move(options.resumeValue_))
    , store_(std::move(options.store_))
    , checkpointer_(std::move(options.checkpointer_))
    , control_(std::move(options.control_))
    , deadline_(std::move(options.deadline_))
{
}

const nlohmann::json& Runtime::context() const noexcept
{
    return context_;
}

const CancellationToken& Runtime::cancellationToken() const noexcept
{
    return cancellationToken_;
}

std::shared_ptr<BaseStore> Runtime::store() const noexcept
{
    return store_;
}

std::shared_ptr<BaseCheckpointSaver> Runtime::checkpointer() const noexcept
{
    return checkpointer_;
}

const nlohmann::json& Runtime::previous() const noexcept
{
    return previous_;
}

const ExecutionInfo& Runtime::executionInfo() const noexcept
{
    return executionInfo_;
}

bool Runtime::drainRequested() const
{
    return control_ && control_->drainRequested();
}

std::optional<std::string> Runtime::drainReason() const
{
    if (!control_)
        return std::nullopt;
    return control_->drainReason();
}

void Runtime::heartbeat() const noexcept
{
}

std::optional<std::chrono::steady_clock::time_point> Runtime::deadline() const noexcept
{
    return deadline_;
}

const StreamWriter& Runtime::streamWriter() const noexcept
{
    return streamWriter_;
}

bool Runtime::hasResumeValue() const noexcept
{
    return resumeValue_.has_value();
}

const nlohmann::json& Runtime::resumeValue() const
{
    if (!resumeValue_.has_value())
        throw std::logic_error("runtime has no resume value");
    return *resumeValue_;
}

Result<nlohmann::json> Runtime::interrupt(
    std::string id,
    nlohmann::json value)
{
    if (id.empty())
        return Status::invalidArgument("interrupt id cannot be empty");

    const std::size_t cursor = interruptCursor_++;
    if (resumeValue_.has_value()) {
        const auto& value = *resumeValue_;
        if (value.is_object() && value.contains(id)) {
            fulfilledInterruptValues_[id] = value.at(id);
            return value.at(id);
        }
        if (value.is_array() && cursor < value.size()) {
            fulfilledInterruptValues_[id] = value.at(cursor);
            return value.at(cursor);
        }
        if (cursor == 0U) {
            fulfilledInterruptValues_[id] = value;
            return value;
        }
    }

    nlohmann::json request {
        { "id", std::move(id) },
        { "value", std::move(value) },
    };
    if (!fulfilledInterruptValues_.empty())
        request["resume_values"] = fulfilledInterruptValues_;
    requestedInterrupts_.push_back(std::move(request));
    return Status::aborted("interrupt requested");
}

const std::vector<nlohmann::json>& Runtime::requestedInterrupts() const noexcept
{
    return requestedInterrupts_;
}

} // namespace lc
