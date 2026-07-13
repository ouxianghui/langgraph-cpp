#include "foundation/executor/inline_executor.hpp"

#include <exception>
#include <string>
#include <utility>

namespace lc {
namespace {

thread_local const void* tlsInlineExecutor = nullptr;

class InlineExecutionScope final {
public:
    explicit InlineExecutionScope(const void* executor) noexcept
        : previous_(tlsInlineExecutor)
    {
        tlsInlineExecutor = executor;
    }

    ~InlineExecutionScope() { tlsInlineExecutor = previous_; }

    InlineExecutionScope(const InlineExecutionScope&) = delete;
    InlineExecutionScope& operator=(const InlineExecutionScope&) = delete;

private:
    const void* previous_ { nullptr };
};

} // namespace

InlineExecutor::InlineExecutor(std::shared_ptr<ILogger> logger)
    : logger_(std::move(logger))
{
}

Status InlineExecutor::runAccepted(Task task, std::source_location from)
{
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return Status::unavailable("executor is closed");
        ++outstanding_;
    }

    const auto status = runTask(std::move(task), from);
    finishTask();
    return status;
}

Status InlineExecutor::post(Task task, std::source_location)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");

    if (isClosed())
        return Status::unavailable("executor is closed");

    return Status::unimplemented("inline executor does not support deferred execution");
}

Status InlineExecutor::postDelayed(Duration, Task task, std::source_location)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");

    if (isClosed())
        return Status::unavailable("executor is closed");

    return Status::unimplemented("inline executor does not support posted execution");
}

Status InlineExecutor::execute(Task task, std::source_location from)
{
    if (!task)
        return Status::invalidArgument("executor task must not be empty");

    if (isExecutorThread())
        return runTask(std::move(task), from);

    return runAccepted(std::move(task), from);
}

Status InlineExecutor::executeAndWait(Task task, std::source_location from)
{
    return execute(std::move(task), from);
}

Status InlineExecutor::waitIdle(Duration timeout)
{
    std::unique_lock lock(mutex_);
    if (timeout <= Duration::zero()) {
        return outstanding_ == 0ULL
            ? Status::ok()
            : Status::deadlineExceeded("executor did not become idle before timeout");
    }

    if (!idleCv_.wait_for(lock, timeout, [this] { return outstanding_ == 0ULL; }))
        return Status::deadlineExceeded("executor did not become idle before timeout");
    return Status::ok();
}

Status InlineExecutor::close(Duration waitIdleTimeout)
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }

    return waitIdle(waitIdleTimeout);
}

bool InlineExecutor::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

bool InlineExecutor::isExecutorThread() const noexcept
{
    return tlsInlineExecutor == static_cast<const void*>(this);
}

Status InlineExecutor::runTask(Task task, std::source_location from)
{
    try {
        InlineExecutionScope scope(this);
        task();
        return Status::ok();
    } catch (const std::exception& ex) {
        try {
            logTo(logger_,
                LogLevel::Warn,
                "InlineExecutor",
                "task exception at {}:{} `{}`: {}",
                __FILE__,
                __LINE__,
                from.file_name(),
                from.line(),
                from.function_name(),
                ex.what());
        } catch (...) {
        }
        return Status::internal(std::string("executor task threw: ") + ex.what());
    } catch (...) {
        try {
            logTo(logger_,
                LogLevel::Warn,
                "InlineExecutor",
                "task exception (non-std) at {}:{} `{}`",
                __FILE__,
                __LINE__,
                from.file_name(),
                from.line(),
                from.function_name());
        } catch (...) {
        }
        return Status::internal("executor task threw a non-std exception");
    }
}

void InlineExecutor::finishTask() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (outstanding_ > 0ULL)
            --outstanding_;
    }
    idleCv_.notify_all();
}

std::shared_ptr<IExecutor> makeInlineExecutor(std::shared_ptr<ILogger> logger)
{
    return std::make_shared<InlineExecutor>(std::move(logger));
}

} // namespace lc
