#include "core/lifecycle/lifecycle_adapters.hpp"

#include <stdexcept>
#include <utility>

namespace lc {
namespace {

class ExecutorLifecycleComponent final : public NamedLifecycleComponent {
public:
    ExecutorLifecycleComponent(std::string name, std::shared_ptr<IExecutor> executor)
        : NamedLifecycleComponent(std::move(name))
        , executor_(std::move(executor))
    {
        if (!executor_)
            throw std::invalid_argument("executor lifecycle component requires an executor");
    }

    [[nodiscard]] Status start() override
    {
        if (executor_->isClosed())
            return Status::failedPrecondition("executor is already closed");
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Clock::Duration timeout) override
    {
        return executor_->waitIdle(timeout);
    }

    [[nodiscard]] Status close(Clock::Duration waitIdleTimeout) override
    {
        if (executor_->isClosed())
            return Status::ok();
        return executor_->close(waitIdleTimeout);
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return executor_->isClosed();
    }

private:
    std::shared_ptr<IExecutor> executor_;
};

class SchedulerLifecycleComponent final : public NamedLifecycleComponent {
public:
    SchedulerLifecycleComponent(std::string name, std::shared_ptr<ITaskScheduler> scheduler)
        : NamedLifecycleComponent(std::move(name))
        , scheduler_(std::move(scheduler))
    {
        if (!scheduler_)
            throw std::invalid_argument("scheduler lifecycle component requires a scheduler");
    }

    [[nodiscard]] Status start() override
    {
        if (scheduler_->isClosed())
            return Status::failedPrecondition("scheduler is already closed");
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Clock::Duration timeout) override
    {
        return scheduler_->waitIdle(timeout);
    }

    [[nodiscard]] Status close(Clock::Duration waitIdleTimeout) override
    {
        if (scheduler_->isClosed())
            return Status::ok();
        return scheduler_->close(waitIdleTimeout);
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return scheduler_->isClosed();
    }

private:
    std::shared_ptr<ITaskScheduler> scheduler_;
};

class EventSinkLifecycleComponent final : public NamedLifecycleComponent {
public:
    EventSinkLifecycleComponent(std::string name, std::shared_ptr<IEventSink> sink)
        : NamedLifecycleComponent(std::move(name))
        , sink_(std::move(sink))
    {
        if (!sink_)
            throw std::invalid_argument("event sink lifecycle component requires an event sink");
    }

    [[nodiscard]] Status start() override
    {
        if (sink_->isClosed())
            return Status::failedPrecondition("event sink is already closed");
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Clock::Duration timeout) override
    {
        return sink_->waitIdle(timeout);
    }

    [[nodiscard]] Status close(Clock::Duration waitIdleTimeout) override
    {
        if (sink_->isClosed())
            return Status::ok();
        return sink_->close(waitIdleTimeout);
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return sink_->isClosed();
    }

private:
    std::shared_ptr<IEventSink> sink_;
};

class StorageLifecycleComponent final : public NamedLifecycleComponent {
public:
    StorageLifecycleComponent(std::string name, std::shared_ptr<IStorage> storage)
        : NamedLifecycleComponent(std::move(name))
        , storage_(std::move(storage))
    {
        if (!storage_)
            throw std::invalid_argument("storage lifecycle component requires storage");
    }

    [[nodiscard]] Status start() override
    {
        if (storage_->isClosed())
            return Status::failedPrecondition("storage is already closed");
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Clock::Duration) override { return storage_->flush(); }

    [[nodiscard]] Status close(Clock::Duration) override
    {
        if (storage_->isClosed())
            return Status::ok();
        return storage_->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return storage_->isClosed();
    }

private:
    std::shared_ptr<IStorage> storage_;
};

class MetricLifecycleComponent final : public NamedLifecycleComponent {
public:
    MetricLifecycleComponent(std::string name, std::shared_ptr<IMetricRecorder> metrics)
        : NamedLifecycleComponent(std::move(name))
        , metrics_(std::move(metrics))
    {
        if (!metrics_)
            throw std::invalid_argument("metric lifecycle component requires metrics");
    }

    [[nodiscard]] Status start() override
    {
        if (metrics_->isClosed())
            return Status::failedPrecondition("metrics recorder is already closed");
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Clock::Duration) override { return metrics_->flush(); }

    [[nodiscard]] Status close(Clock::Duration) override
    {
        if (metrics_->isClosed())
            return Status::ok();
        if (auto status = metrics_->flush(); !status.isOk())
            return status;
        return metrics_->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return metrics_->isClosed();
    }

private:
    std::shared_ptr<IMetricRecorder> metrics_;
};

class TraceSinkLifecycleComponent final : public NamedLifecycleComponent {
public:
    TraceSinkLifecycleComponent(std::string name, std::shared_ptr<ITraceSink> traceSink)
        : NamedLifecycleComponent(std::move(name))
        , traceSink_(std::move(traceSink))
    {
        if (!traceSink_)
            throw std::invalid_argument("trace sink lifecycle component requires a trace sink");
    }

    [[nodiscard]] Status start() override
    {
        if (traceSink_->isClosed())
            return Status::failedPrecondition("trace sink is already closed");
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Clock::Duration) override { return traceSink_->flush(); }

    [[nodiscard]] Status close(Clock::Duration) override
    {
        if (traceSink_->isClosed())
            return Status::ok();
        if (auto status = traceSink_->flush(); !status.isOk())
            return status;
        return traceSink_->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return traceSink_->isClosed();
    }

private:
    std::shared_ptr<ITraceSink> traceSink_;
};

class HttpClientLifecycleComponent final : public NamedLifecycleComponent {
public:
    HttpClientLifecycleComponent(std::string name, std::shared_ptr<IHttpClient> httpClient)
        : NamedLifecycleComponent(std::move(name))
        , httpClient_(std::move(httpClient))
    {
        if (!httpClient_)
            throw std::invalid_argument("HTTP client lifecycle component requires an HTTP client");
    }

    [[nodiscard]] Status start() override
    {
        if (httpClient_->isClosed())
            return Status::failedPrecondition("HTTP client is already closed");
        return Status::ok();
    }

    [[nodiscard]] Status waitIdle(Clock::Duration) override { return Status::ok(); }

    [[nodiscard]] Status close(Clock::Duration) override
    {
        if (httpClient_->isClosed())
            return Status::ok();
        return httpClient_->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return httpClient_->isClosed();
    }

private:
    std::shared_ptr<IHttpClient> httpClient_;
};

} // namespace

std::shared_ptr<ILifecycle> lifecycleComponent(
    std::string name,
    std::shared_ptr<IExecutor> executor)
{
    return std::make_shared<ExecutorLifecycleComponent>(std::move(name), std::move(executor));
}

std::shared_ptr<ILifecycle> lifecycleComponent(
    std::string name,
    std::shared_ptr<ITaskScheduler> scheduler)
{
    return std::make_shared<SchedulerLifecycleComponent>(std::move(name), std::move(scheduler));
}

std::shared_ptr<ILifecycle> lifecycleComponent(
    std::string name,
    std::shared_ptr<IEventSink> eventSink)
{
    return std::make_shared<EventSinkLifecycleComponent>(std::move(name), std::move(eventSink));
}

std::shared_ptr<ILifecycle> lifecycleComponent(
    std::string name,
    std::shared_ptr<IStorage> storage)
{
    return std::make_shared<StorageLifecycleComponent>(std::move(name), std::move(storage));
}

std::shared_ptr<ILifecycle> lifecycleComponent(
    std::string name,
    std::shared_ptr<IMetricRecorder> metrics)
{
    return std::make_shared<MetricLifecycleComponent>(std::move(name), std::move(metrics));
}

std::shared_ptr<ILifecycle> lifecycleComponent(
    std::string name,
    std::shared_ptr<ITraceSink> traceSink)
{
    return std::make_shared<TraceSinkLifecycleComponent>(std::move(name), std::move(traceSink));
}

std::shared_ptr<ILifecycle> lifecycleComponent(
    std::string name,
    std::shared_ptr<IHttpClient> httpClient)
{
    return std::make_shared<HttpClientLifecycleComponent>(std::move(name), std::move(httpClient));
}

} // namespace lc
