#include "langgraph/graph/stream_state.hh"

#include <chrono>
#include <future>
#include <thread>
#include <utility>

namespace lc {
namespace {

template <typename StreamState>
Result<RunResult> waitForResultDraining(StreamState& state)
{
    constexpr auto pollInterval = std::chrono::milliseconds(10);
    while (state.resultFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        auto received = state.channel_.receiveFor(pollInterval);
        if (!received.isOk()) {
            if (received.status().code() == StatusCode::DeadlineExceeded)
                continue;
            return received.status();
        }
        if (!received->has_value())
            (void)state.resultFuture_.wait_for(pollInterval);
    }
    return state.resultFuture_.get();
}

} // namespace

RunEventStreamState::RunEventStreamState(std::size_t capacity)
    : channel_(capacity)
    , resultFuture_(resultPromise_.get_future().share())
{
}

RunEventStreamState::~RunEventStreamState()
{
    close();
}

void RunEventStreamState::close() noexcept
{
    channel_.close();
    if (!worker_.joinable())
        return;
    if (worker_.get_id() == std::this_thread::get_id()) {
        worker_.detach();
        return;
    }
    worker_.join();
}

RunEventStream::RunEventStream(std::shared_ptr<RunEventStreamState> state)
    : state_(std::move(state))
{
}

RunEventStream::~RunEventStream()
{
    close();
}

bool RunEventStream::valid() const noexcept
{
    return static_cast<bool>(state_);
}

Result<std::optional<RuntimeEvent>> RunEventStream::next()
{
    if (!state_)
        return Status::failedPrecondition("run event stream is not valid");
    return state_->channel_.receive();
}

Result<std::optional<RuntimeEvent>> RunEventStream::nextFor(Duration timeout)
{
    if (!state_)
        return Status::failedPrecondition("run event stream is not valid");
    return state_->channel_.receiveFor(timeout);
}

Result<RunResult> RunEventStream::result()
{
    if (!state_)
        return Status::failedPrecondition("run event stream is not valid");
    return waitForResultDraining(*state_);
}

void RunEventStream::close() noexcept
{
    if (state_)
        state_->close();
}

RunPartStreamState::RunPartStreamState(std::size_t capacity)
    : channel_(capacity)
    , resultFuture_(resultPromise_.get_future().share())
{
}

RunPartStreamState::~RunPartStreamState()
{
    close();
}

void RunPartStreamState::close() noexcept
{
    channel_.close();
    if (!worker_.joinable())
        return;
    if (worker_.get_id() == std::this_thread::get_id()) {
        worker_.detach();
        return;
    }
    worker_.join();
}

RunPartStream::RunPartStream(std::shared_ptr<RunPartStreamState> state)
    : state_(std::move(state))
{
}

RunPartStream::~RunPartStream()
{
    close();
}

bool RunPartStream::valid() const noexcept
{
    return static_cast<bool>(state_);
}

Result<std::optional<StreamPart>> RunPartStream::next()
{
    if (!state_)
        return Status::failedPrecondition("run part stream is not valid");
    return state_->channel_.receive();
}

Result<std::optional<StreamPart>> RunPartStream::nextFor(Duration timeout)
{
    if (!state_)
        return Status::failedPrecondition("run part stream is not valid");
    return state_->channel_.receiveFor(timeout);
}

Result<RunResult> RunPartStream::result()
{
    if (!state_)
        return Status::failedPrecondition("run part stream is not valid");
    return waitForResultDraining(*state_);
}

void RunPartStream::close() noexcept
{
    if (state_)
        state_->close();
}

RunStatus runStatusFromStatus(const Status& status) noexcept
{
    if (status.code() == StatusCode::Cancelled)
        return RunStatus::Cancelled;
    if (status.code() == StatusCode::ResourceExhausted
        && status.message().contains("max steps"))
        return RunStatus::MaxStepsExceeded;
    return RunStatus::Failed;
}

} // namespace lc
