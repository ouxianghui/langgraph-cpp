#include "foundation/lifecycle/lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace lc {
namespace {

[[nodiscard]] Status combineStatus(Status current, const Status& next)
{
    if (!current.isOk())
        return current;
    return next;
}

[[nodiscard]] Status componentExceptionStatus(
    std::string_view action,
    std::string_view name,
    const std::exception& error)
{
    std::string message("lifecycle component ");
    message.append(action);
    message.append(" threw name=");
    message.append(name);
    message.append(": ");
    message.append(error.what());
    return Status::internal(std::move(message));
}

[[nodiscard]] Status componentExceptionStatus(std::string_view action, std::string_view name)
{
    std::string message("lifecycle component ");
    message.append(action);
    message.append(" threw name=");
    message.append(name);
    return Status::internal(std::move(message));
}

[[nodiscard]] Status safeStart(const std::shared_ptr<ILifecycle>& component)
{
    try {
        return component->start();
    } catch (const std::exception& error) {
        return componentExceptionStatus("start", component->name(), error);
    } catch (...) {
        return componentExceptionStatus("start", component->name());
    }
}

[[nodiscard]] Status safeWaitIdle(
    const std::shared_ptr<ILifecycle>& component,
    Clock::Duration timeout)
{
    try {
        return component->waitIdle(timeout);
    } catch (const std::exception& error) {
        return componentExceptionStatus("waitIdle", component->name(), error);
    } catch (...) {
        return componentExceptionStatus("waitIdle", component->name());
    }
}

[[nodiscard]] Status safeClose(
    const std::shared_ptr<ILifecycle>& component,
    Clock::Duration timeout)
{
    try {
        return component->close(timeout);
    } catch (const std::exception& error) {
        return componentExceptionStatus("close", component->name(), error);
    } catch (...) {
        return componentExceptionStatus("close", component->name());
    }
}

[[nodiscard]] bool hasDeadline(Clock::Duration timeout) noexcept
{
    return timeout > Clock::Duration::zero() && timeout != Clock::Duration::max();
}

[[nodiscard]] Clock::Duration remainingBudget(
    Clock::Duration timeout,
    std::chrono::steady_clock::time_point startedAt)
{
    if (timeout == Clock::Duration::max())
        return timeout;
    if (timeout <= Clock::Duration::zero())
        return Clock::Duration::zero();

    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    if (elapsed >= timeout)
        return Clock::Duration::zero();
    return timeout - elapsed;
}

[[nodiscard]] bool deadlineExpired(
    Clock::Duration timeout,
    std::chrono::steady_clock::time_point startedAt)
{
    return hasDeadline(timeout) && remainingBudget(timeout, startedAt) <= Clock::Duration::zero();
}

[[nodiscard]] Status lifecycleDeadlineStatus(std::string message)
{
    return Status::deadlineExceeded(std::move(message));
}

} // namespace

NamedLifecycleComponent::NamedLifecycleComponent(std::string name)
    : name_(std::move(name))
{
    if (name_.empty())
        throw std::invalid_argument("lifecycle component name cannot be empty");
}

std::string_view NamedLifecycleComponent::name() const noexcept
{
    return name_;
}

Lifecycle::Lifecycle(std::shared_ptr<ILogger> logger)
    : logger_(std::move(logger))
{
}

Lifecycle::~Lifecycle()
{
    (void)close(CloseOptions {
        .timeout_ = Clock::Duration::zero(),
        .continueOnError_ = true,
    });
}

Status Lifecycle::add(std::shared_ptr<ILifecycle> component)
{
    if (!component)
        return Status::invalidArgument("lifecycle component cannot be null");

    std::lock_guard lock(mutex_);
    if (state_ != LifecycleState::Created)
        return Status::failedPrecondition("cannot add lifecycle component after start");

    const auto duplicate = std::any_of(
        components_.begin(),
        components_.end(),
        [&](const auto& existing) {
            return existing.component_->name() == component->name();
        });
    if (duplicate)
        return Status::alreadyExists("lifecycle component already registered");

    components_.push_back(ComponentRecord {
        .component_ = std::move(component),
    });
    return Status::ok();
}

Status Lifecycle::start()
{
    std::vector<std::pair<std::size_t, std::shared_ptr<ILifecycle>>> components;
    {
        std::unique_lock lock(mutex_);
        while (state_ == LifecycleState::Starting) {
            stateCv_.wait(lock, [this] {
                return state_ != LifecycleState::Starting;
            });
        }

        if (state_ == LifecycleState::Started)
            return Status::ok();
        if (state_ == LifecycleState::Closing || state_ == LifecycleState::Closed)
            return Status::failedPrecondition("lifecycle manager is closed");
        if (state_ == LifecycleState::Failed)
            return startStatus_.isOk() ? Status::failedPrecondition("lifecycle manager has failed") : startStatus_;

        state_ = LifecycleState::Starting;
        startStatus_ = Status::ok();
        for (std::size_t i = 0; i < components_.size(); ++i) {
            components_[i].state_ = LifecycleState::Created;
            components_[i].status_ = Status::ok();
            components.emplace_back(i, components_[i].component_);
        }
    }

    std::vector<std::pair<std::size_t, std::shared_ptr<ILifecycle>>> started;
    for (const auto& [index, component] : components) {
        {
            std::lock_guard lock(mutex_);
            components_[index].state_ = LifecycleState::Starting;
            components_[index].status_ = Status::ok();
        }

        auto status = safeStart(component);
        if (!status.isOk()) {
            logTo(logger_,
                LogLevel::Warn,
                "Lifecycle",
                "component start failed name={} status={}",
                __FILE__,
                __LINE__,
                component->name(),
                status.toString());

            {
                std::lock_guard lock(mutex_);
                components_[index].state_ = LifecycleState::Failed;
                components_[index].status_ = status;
            }

            const auto rollbackStartedAt = std::chrono::steady_clock::now();
            Status rollbackStatus = Status::ok();
            for (auto it = started.rbegin(); it != started.rend(); ++it) {
                const auto remaining = remainingBudget(CloseOptions {}.timeout_, rollbackStartedAt);
                const auto closeStatus = safeClose(it->second, remaining);
                {
                    std::lock_guard lock(mutex_);
                    components_[it->first].state_ = closeStatus.isOk() ? LifecycleState::Closed : LifecycleState::Failed;
                    components_[it->first].status_ = closeStatus;
                }
                if (!closeStatus.isOk()) {
                    logTo(logger_,
                        LogLevel::Warn,
                        "Lifecycle",
                        "component rollback close failed name={} status={}",
                        __FILE__,
                        __LINE__,
                        it->second->name(),
                        closeStatus.toString());
                    rollbackStatus = combineStatus(std::move(rollbackStatus), closeStatus);
                }
            }

            if (!rollbackStatus.isOk()) {
                logTo(logger_,
                    LogLevel::Warn,
                    "Lifecycle",
                    "start rollback finished with failure status={}",
                    __FILE__,
                    __LINE__,
                    rollbackStatus.toString());
            }

            {
                std::lock_guard lock(mutex_);
                state_ = LifecycleState::Failed;
                startStatus_ = status;
            }
            stateCv_.notify_all();
            return status;
        }

        {
            std::lock_guard lock(mutex_);
            components_[index].state_ = LifecycleState::Started;
            components_[index].status_ = Status::ok();
        }
        started.emplace_back(index, component);
    }

    {
        std::lock_guard lock(mutex_);
        state_ = LifecycleState::Started;
        startStatus_ = Status::ok();
    }
    stateCv_.notify_all();
    return Status::ok();
}

Status Lifecycle::waitIdle(Clock::Duration timeout)
{
    const auto startedAt = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<ILifecycle>> components;
    {
        std::unique_lock lock(mutex_);
        while (state_ == LifecycleState::Starting) {
            if (deadlineExpired(timeout, startedAt))
                return lifecycleDeadlineStatus("lifecycle did not finish start before waitIdle timeout");

            const auto remaining = remainingBudget(timeout, startedAt);
            if (remaining == Clock::Duration::max()) {
                stateCv_.wait(lock, [this] {
                    return state_ != LifecycleState::Starting;
                });
            } else {
                const auto ready = stateCv_.wait_for(lock, remaining, [this] {
                    return state_ != LifecycleState::Starting;
                });
                if (!ready)
                    return lifecycleDeadlineStatus("lifecycle did not finish start before waitIdle timeout");
            }
        }
        components.reserve(components_.size());
        for (const auto& record : components_)
            components.push_back(record.component_);
    }

    for (const auto& component : components) {
        if (deadlineExpired(timeout, startedAt)) {
            auto status = lifecycleDeadlineStatus("lifecycle did not become idle before timeout");
            logTo(logger_,
                LogLevel::Warn,
                "Lifecycle",
                "waitIdle timeout component={} status={}",
                __FILE__,
                __LINE__,
                component->name(),
                status.toString());
            return status;
        }
        auto status = safeWaitIdle(component, remainingBudget(timeout, startedAt));
        if (!status.isOk()) {
            logTo(logger_,
                LogLevel::Warn,
                "Lifecycle",
                "component waitIdle failed name={} status={}",
                __FILE__,
                __LINE__,
                component->name(),
                status.toString());
            return status;
        }
    }
    return Status::ok();
}

Status Lifecycle::close(CloseOptions options)
{
    const auto startedAt = std::chrono::steady_clock::now();
    std::vector<std::pair<std::size_t, std::shared_ptr<ILifecycle>>> components;
    {
        std::unique_lock lock(mutex_);
        while (state_ == LifecycleState::Starting || closing_) {
            if (deadlineExpired(options.timeout_, startedAt)) {
                return lifecycleDeadlineStatus(
                    state_ == LifecycleState::Starting
                        ? "lifecycle did not finish start before close timeout"
                        : "lifecycle close already in progress");
            }

            const auto remaining = remainingBudget(options.timeout_, startedAt);
            if (remaining == Clock::Duration::max()) {
                stateCv_.wait(lock, [this] {
                    return state_ != LifecycleState::Starting && !closing_;
                });
            } else {
                const auto ready = stateCv_.wait_for(lock, remaining, [this] {
                    return state_ != LifecycleState::Starting && !closing_;
                });
                if (!ready) {
                    return lifecycleDeadlineStatus(
                        state_ == LifecycleState::Starting
                            ? "lifecycle did not finish start before close timeout"
                            : "lifecycle close already in progress");
                }
            }
        }

        if (closeAttempted_)
            return closeStatus_;

        closing_ = true;
        state_ = LifecycleState::Closing;
        for (std::size_t i = 0; i < components_.size(); ++i) {
            if (components_[i].state_ != LifecycleState::Closed)
                components.emplace_back(i, components_[i].component_);
        }
    }

    Status finalStatus = Status::ok();
    for (auto it = components.rbegin(); it != components.rend(); ++it) {
        const auto& [index, component] = *it;
        {
            std::lock_guard lock(mutex_);
            components_[index].state_ = LifecycleState::Closing;
            components_[index].status_ = Status::ok();
        }

        Status status = Status::ok();
        if (deadlineExpired(options.timeout_, startedAt)) {
            status = lifecycleDeadlineStatus("lifecycle close exceeded timeout");
        } else {
            status = safeClose(component, remainingBudget(options.timeout_, startedAt));
        }

        if (!status.isOk()) {
            logTo(logger_,
                LogLevel::Warn,
                "Lifecycle",
                "component close failed name={} status={}",
                __FILE__,
                __LINE__,
                component->name(),
                status.toString());
            finalStatus = combineStatus(std::move(finalStatus), status);
            {
                std::lock_guard lock(mutex_);
                components_[index].state_ = LifecycleState::Failed;
                components_[index].status_ = status;
            }
            if (!options.continueOnError_) {
                std::lock_guard lock(mutex_);
                state_ = LifecycleState::Failed;
                closeStatus_ = status;
                closeAttempted_ = true;
                closing_ = false;
                stateCv_.notify_all();
                return closeStatus_;
            }
            continue;
        }

        {
            std::lock_guard lock(mutex_);
            components_[index].state_ = LifecycleState::Closed;
            components_[index].status_ = Status::ok();
        }
    }

    {
        std::lock_guard lock(mutex_);
        state_ = finalStatus.isOk() ? LifecycleState::Closed : LifecycleState::Failed;
        closeStatus_ = finalStatus;
        closeAttempted_ = true;
        closing_ = false;
    }
    stateCv_.notify_all();
    if (!closeStatus_.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "Lifecycle",
            "close finished with failure status={}",
            __FILE__,
            __LINE__,
            closeStatus_.toString());
    }
    return closeStatus_;
}

bool Lifecycle::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return state_ == LifecycleState::Closed;
}

LifecycleState Lifecycle::state() const noexcept
{
    std::lock_guard lock(mutex_);
    return state_;
}

std::vector<ComponentStatus> Lifecycle::components() const
{
    std::lock_guard lock(mutex_);
    std::vector<ComponentStatus> out;
    out.reserve(components_.size());
    for (const auto& record : components_) {
        out.push_back(ComponentStatus {
            .name_ = std::string(record.component_->name()),
            .state_ = record.state_,
            .closed_ = record.component_->isClosed(),
            .status_ = record.status_,
        });
    }
    return out;
}

std::size_t Lifecycle::count() const noexcept
{
    std::lock_guard lock(mutex_);
    return components_.size();
}

std::string_view stateName(LifecycleState state) noexcept
{
    switch (state) {
    case LifecycleState::Created:
        return "created";
    case LifecycleState::Starting:
        return "starting";
    case LifecycleState::Started:
        return "started";
    case LifecycleState::Closing:
        return "closing";
    case LifecycleState::Closed:
        return "closed";
    case LifecycleState::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace lc
