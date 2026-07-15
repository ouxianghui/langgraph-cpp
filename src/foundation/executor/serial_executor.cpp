#include "foundation/executor/serial_executor.hpp"

#include "foundation/threading/thread.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] Status unavailableFromThreadStopped(const std::exception& error)
{
    return Status::unavailable(std::string("serial executor is closed: ") + error.what());
}

} // namespace

SerialExecutor::SerialExecutor(std::shared_ptr<IThread> thread)
    : thread_(std::move(thread))
{
    if (!thread_)
        throw std::invalid_argument("SerialExecutor requires a non-null thread");
}

SerialExecutor::SerialExecutor(std::size_t maxPendingDispatch, std::shared_ptr<ILogger> logger)
    : SerialExecutor(std::make_shared<Thread>(maxPendingDispatch, std::move(logger)))
{
}

SerialExecutor::SerialExecutor(
    std::string_view name,
    std::size_t maxPendingDispatch,
    std::shared_ptr<ILogger> logger)
    : SerialExecutor(std::make_shared<Thread>(name, maxPendingDispatch, std::move(logger)))
{
}

Status SerialExecutor::enqueue(Task task, std::source_location from)
{
    if (isClosed())
        return Status::unavailable("serial executor is closed");

    thread_->dispatchAsync(std::move(task), from);
    return Status::ok();
}

Status SerialExecutor::post(Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");
    return enqueue(std::move(task), from);
}

Status SerialExecutor::postDelayed(Duration delay, Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");
    if (isClosed())
        return Status::unavailable("serial executor is closed");

    thread_->dispatchAfter(delay, std::move(task), from);
    return Status::ok();
}

Status SerialExecutor::execute(Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");
    if (isExecutorThread())
        return runTask(std::move(task));

    return post(std::move(task), from);
}

Status SerialExecutor::executeAndWait(Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");
    if (isExecutorThread())
        return runTask(std::move(task));
    if (isClosed())
        return Status::unavailable("serial executor is closed");

    Status result = Status::internal("serial executor task did not run");
    try {
        thread_->dispatchSync([task = std::move(task), &result, this]() mutable {
            result = runTask(std::move(task));
        },
            from);
    } catch (const ThreadStopped& error) {
        return unavailableFromThreadStopped(error);
    } catch (const std::exception& error) {
        return Status::internal(std::string("serial executor executeAndWait failed: ") + error.what());
    } catch (...) {
        return Status::internal("serial executor executeAndWait failed with a non-std exception");
    }

    return result;
}

Status SerialExecutor::waitIdle(Duration timeout)
{
    return thread_->waitIdle(timeout);
}

Status SerialExecutor::close(Duration waitIdleTimeout)
{
    return thread_->shutdown(waitIdleTimeout);
}

bool SerialExecutor::isClosed() const noexcept
{
    return !thread_ || !thread_->isRunning();
}

bool SerialExecutor::isExecutorThread() const noexcept
{
    return thread_ && thread_->isCurrentThread();
}

std::shared_ptr<IThread> SerialExecutor::thread() const noexcept
{
    return thread_;
}

Status SerialExecutor::runTask(Task task)
{
    try {
        task();
        return Status::ok();
    } catch (const std::exception& error) {
        return Status::internal(std::string("executor task threw: ") + error.what());
    } catch (...) {
        return Status::internal("executor task threw a non-std exception");
    }
}

std::shared_ptr<IExecutor> makeSerialExecutor(std::size_t maxPendingDispatch, std::shared_ptr<ILogger> logger)
{
    return std::make_shared<SerialExecutor>(maxPendingDispatch, std::move(logger));
}

std::shared_ptr<IExecutor> makeSerialExecutor(
    std::string_view name,
    std::size_t maxPendingDispatch,
    std::shared_ptr<ILogger> logger)
{
    return std::make_shared<SerialExecutor>(name, maxPendingDispatch, std::move(logger));
}

std::shared_ptr<IExecutor> makeSerialExecutor(std::shared_ptr<IThread> thread)
{
    return std::make_shared<SerialExecutor>(std::move(thread));
}

} // namespace lgc
