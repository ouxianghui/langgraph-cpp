#include "foundation/executor/concurrent_executor.hpp"

#include "foundation/threading/thread_pool.hpp"

#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] Status runExecutorTask(IExecutor::Task task)
{
    try {
        task();
        return Status::ok();
    } catch (const std::exception& ex) {
        return Status::internal(std::string("executor task threw: ") + ex.what());
    } catch (...) {
        return Status::internal("executor task threw a non-std exception");
    }
}

} // namespace

ConcurrentExecutor::ConcurrentExecutor(std::shared_ptr<IThreadPool> pool)
    : pool_(std::move(pool))
{
    if (!pool_)
        throw std::invalid_argument("ConcurrentExecutor requires a non-null thread pool");
    closed_.store(!pool_->isRunning(), std::memory_order_release);
}

ConcurrentExecutor::ConcurrentExecutor(
    std::size_t threadCount,
    std::size_t maxPendingSubmit,
    std::shared_ptr<ILogger> logger)
    : ConcurrentExecutor(std::make_shared<ThreadPool>(threadCount, maxPendingSubmit, std::move(logger)))
{
}

Status ConcurrentExecutor::enqueue(Task task, std::source_location from)
{
    if (isClosed())
        return Status::unavailable("executor is closed");

    auto status = pool_->submit(std::move(task), from);
    if (status.isOk())
        return status;

    if (!pool_->isRunning()) {
        closed_.store(true, std::memory_order_release);
        return Status::unavailable("executor is closed");
    }

    return status;
}

Status ConcurrentExecutor::post(Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");
    return enqueue(std::move(task), from);
}

Status ConcurrentExecutor::postDelayed(Duration delay, Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");

    if (isClosed())
        return Status::unavailable("executor is closed");

    if (delay > Duration::zero())
        return Status::unimplemented("concurrent executor does not support delayed execution");

    return post(std::move(task), from);
}

Status ConcurrentExecutor::execute(Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");

    if (isExecutorThread())
        return runExecutorTask(std::move(task));

    return post(std::move(task), from);
}

Status ConcurrentExecutor::executeAndWait(Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");

    if (isExecutorThread())
        return runExecutorTask(std::move(task));

    auto promise = std::make_shared<std::promise<Status>>();
    auto future = promise->get_future();
    auto status = enqueue(
        [task = std::move(task), promise]() mutable {
            try {
                promise->set_value(runExecutorTask(std::move(task)));
            } catch (...) {
                try {
                    promise->set_exception(std::current_exception());
                } catch (...) {
                }
            }
        },
        from);

    if (!status.isOk())
        return status;

    promise.reset();
    try {
        return future.get();
    } catch (const std::future_error& ex) {
        return Status::aborted(std::string("executor task did not complete: ") + ex.what());
    } catch (const std::exception& ex) {
        return Status::internal(std::string("executor executeAndWait failed: ") + ex.what());
    } catch (...) {
        return Status::internal("executor executeAndWait failed with a non-std exception");
    }
}

Status ConcurrentExecutor::waitIdle(Duration timeout)
{
    return pool_->waitIdle(timeout);
}

Status ConcurrentExecutor::close(Duration waitIdleTimeout)
{
    const bool wasClosed = closed_.exchange(true, std::memory_order_acq_rel);
    if (wasClosed)
        return Status::ok();

    return pool_->shutdown(waitIdleTimeout);
}

bool ConcurrentExecutor::isClosed() const noexcept
{
    return closed_.load(std::memory_order_acquire) || !pool_->isRunning();
}

bool ConcurrentExecutor::isExecutorThread() const noexcept
{
    return pool_->isWorkerThread();
}

std::shared_ptr<IThreadPool> ConcurrentExecutor::threadPool() const noexcept
{
    return pool_;
}

std::shared_ptr<IExecutor> makeConcurrentExecutor(
    std::size_t threadCount,
    std::size_t maxPendingSubmit,
    std::shared_ptr<ILogger> logger)
{
    return std::make_shared<ConcurrentExecutor>(threadCount, maxPendingSubmit, std::move(logger));
}

std::shared_ptr<IExecutor> makeConcurrentExecutor(std::shared_ptr<IThreadPool> pool)
{
    return std::make_shared<ConcurrentExecutor>(std::move(pool));
}

} // namespace lgc
