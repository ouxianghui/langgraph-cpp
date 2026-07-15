#include "core/runtime/runtime_services.hpp"

#include "core/lifecycle/lifecycle_components.hpp"
#include "foundation/blob/blob_store.hpp"
#include "foundation/cache/cache.hpp"
#include "foundation/event/memory_event_sink.hpp"
#include "foundation/executor/concurrent_executor.hpp"
#include "foundation/scheduler/scheduler.hpp"
#include "foundation/secrets/secret_provider.hpp"
#include "foundation/storage/memory_storage.hpp"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] Status requireService(bool required, bool present, std::string_view name)
{
    if (!required || present)
        return Status::ok();
    return Status::failedPrecondition("runtime service is not configured: " + std::string(name));
}

[[nodiscard]] Status addLifecycleComponent(
    Lifecycle& lifecycle,
    std::shared_ptr<ILifecycle> component)
{
    if (!component)
        return Status::ok();
    return lifecycle.add(std::move(component));
}

} // namespace

RuntimeServiceRequirements RuntimeServiceRequirements::core() noexcept
{
    return RuntimeServiceRequirements {};
}

RuntimeServiceRequirements RuntimeServiceRequirements::all() noexcept
{
    return RuntimeServiceRequirements {
        .logger_ = true,
        .storage_ = true,
        .blobStore_ = true,
        .cache_ = true,
        .executor_ = true,
        .scheduler_ = true,
        .httpClient_ = true,
        .secrets_ = true,
        .eventSink_ = true,
        .metrics_ = true,
        .traceSink_ = true,
    };
}

Status RuntimeServices::validate(RuntimeServiceRequirements requirements) const
{
    if (auto status = requireService(requirements.logger_, static_cast<bool>(logger_), "logger"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.storage_, static_cast<bool>(storage_), "storage"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.blobStore_, static_cast<bool>(blobStore_), "blobStore"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.cache_, static_cast<bool>(cache_), "cache"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.executor_, static_cast<bool>(executor_), "executor"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.scheduler_, static_cast<bool>(scheduler_), "scheduler"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.httpClient_, static_cast<bool>(httpClient_), "httpClient"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.secrets_, static_cast<bool>(secrets_), "secrets"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.eventSink_, static_cast<bool>(eventSink_), "eventSink"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.metrics_, static_cast<bool>(metrics_), "metrics"); !status.isOk())
        return status;
    if (auto status = requireService(requirements.traceSink_, static_cast<bool>(traceSink_), "traceSink"); !status.isOk())
        return status;

    if (logger_ && logger_->isClosed())
        return Status::failedPrecondition("runtime logger is closed");
    if (storage_ && storage_->isClosed())
        return Status::failedPrecondition("runtime storage is closed");
    if (executor_ && executor_->isClosed())
        return Status::failedPrecondition("runtime executor is closed");
    if (scheduler_ && scheduler_->isClosed())
        return Status::failedPrecondition("runtime scheduler is closed");
    if (httpClient_ && httpClient_->isClosed())
        return Status::failedPrecondition("runtime HTTP client is closed");
    if (eventSink_ && eventSink_->isClosed())
        return Status::failedPrecondition("runtime event sink is closed");
    if (metrics_ && metrics_->isClosed())
        return Status::failedPrecondition("runtime metrics recorder is closed");
    if (traceSink_ && traceSink_->isClosed())
        return Status::failedPrecondition("runtime trace sink is closed");

    return Status::ok();
}

Result<std::shared_ptr<Lifecycle>> RuntimeServices::createLifecycle() const
{
    auto lifecycle = std::make_shared<Lifecycle>(logger_);

    try {
        if (storage_) {
            if (auto status = addLifecycleComponent(*lifecycle, makeLifecycleComponent("storage", storage_)); !status.isOk())
                return status;
        }
        if (metrics_) {
            if (auto status = addLifecycleComponent(*lifecycle, makeLifecycleComponent("metrics", metrics_)); !status.isOk())
                return status;
        }
        if (traceSink_) {
            if (auto status = addLifecycleComponent(*lifecycle, makeLifecycleComponent("trace", traceSink_)); !status.isOk())
                return status;
        }
        if (eventSink_) {
            if (auto status = addLifecycleComponent(*lifecycle, makeLifecycleComponent("events", eventSink_)); !status.isOk())
                return status;
        }
        if (httpClient_) {
            if (auto status = addLifecycleComponent(*lifecycle, makeLifecycleComponent("http", httpClient_)); !status.isOk())
                return status;
        }
        if (executor_) {
            if (auto status = addLifecycleComponent(*lifecycle, makeLifecycleComponent("executor", executor_)); !status.isOk())
                return status;
        }
        if (scheduler_) {
            if (auto status = addLifecycleComponent(*lifecycle, makeLifecycleComponent("scheduler", scheduler_)); !status.isOk())
                return status;
        }
    } catch (const std::exception& error) {
        std::string message("failed to create runtime lifecycle: ");
        message.append(error.what());
        return Status::internal(std::move(message));
    } catch (...) {
        return Status::internal("failed to create runtime lifecycle");
    }

    return lifecycle;
}

RuntimeServices defaultRuntimeServices()
{
    RuntimeServices services;
    services.logger_ = Logger::defaultLogger();
    services.storage_ = std::make_shared<MemoryStorage>();
    services.blobStore_ = std::make_shared<MemoryBlobStore>();
    services.cache_ = std::make_shared<MemoryCache>();
    services.executor_ = std::make_shared<ConcurrentExecutor>(0, 0, services.logger_);
    services.secrets_ = std::make_shared<MemorySecrets>();
    services.eventSink_ = std::make_shared<MemoryEventSink>();
    services.metrics_ = std::make_shared<InMemoryMetricRecorder>();
    services.traceSink_ = std::make_shared<InMemoryTraceSink>();
    services.scheduler_ = std::make_shared<TaskScheduler>(SchedulerOptions {
        .executor_ = services.executor_,
        .metricsRecorder_ = services.metrics_,
        .traceSink_ = services.traceSink_,
    });
    return services;
}

} // namespace lgc
