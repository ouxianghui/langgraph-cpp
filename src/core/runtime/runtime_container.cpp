#include "core/runtime/runtime_container.hpp"

#include <utility>

namespace lc {

RuntimeContainer::RuntimeContainer(RuntimeServices services, RuntimeContainerOptions options)
    : services_(std::move(services))
    , options_(std::move(options))
{
}

RuntimeContainer::~RuntimeContainer()
{
    (void)close(options_.closeOptions_);
}

const RuntimeServices& RuntimeContainer::services() const noexcept
{
    return services_;
}

RuntimeServices& RuntimeContainer::services() noexcept
{
    return services_;
}

Status RuntimeContainer::validate() const
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::failedPrecondition("runtime container is closed");
    return services_.validate(options_.requirements_);
}

Status RuntimeContainer::start()
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::failedPrecondition("runtime container is closed");
    if (auto status = ensureLifecycleLocked(); !status.isOk())
        return status;
    return lifecycle_->start();
}

Status RuntimeContainer::waitIdle(Clock::Duration timeout)
{
    std::shared_ptr<Lifecycle> lifecycle;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return Status::ok();
        if (auto status = ensureLifecycleLocked(); !status.isOk())
            return status;
        lifecycle = lifecycle_;
    }
    return lifecycle->waitIdle(timeout);
}

Status RuntimeContainer::close(CloseOptions options)
{
    std::shared_ptr<Lifecycle> lifecycle;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return Status::ok();
        closed_ = true;
        lifecycle = lifecycle_;
    }
    if (!lifecycle)
        return Status::ok();
    return lifecycle->close(options);
}

bool RuntimeContainer::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_ || (lifecycle_ && lifecycle_->isClosed());
}

std::shared_ptr<Lifecycle> RuntimeContainer::lifecycle() const
{
    std::lock_guard lock(mutex_);
    return lifecycle_;
}

Status RuntimeContainer::ensureLifecycleLocked()
{
    if (lifecycle_)
        return Status::ok();
    if (auto status = services_.validate(options_.requirements_); !status.isOk())
        return status;
    auto lifecycle = services_.createLifecycle();
    if (!lifecycle.isOk())
        return lifecycle.status();
    lifecycle_ = std::move(*lifecycle);
    return Status::ok();
}

Result<std::shared_ptr<RuntimeContainer>> createRuntimeContainer(
    RuntimeServices services,
    RuntimeContainerOptions options)
{
    auto container = std::make_shared<RuntimeContainer>(std::move(services), std::move(options));
    if (auto status = container->validate(); !status.isOk())
        return status;
    return container;
}

} // namespace lc
