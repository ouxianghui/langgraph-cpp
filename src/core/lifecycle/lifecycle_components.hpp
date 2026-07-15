#pragma once

#include "foundation/event/i_event_sink.hpp"
#include "foundation/executor/i_executor.hpp"
#include "foundation/lifecycle/lifecycle.hpp"
#include "foundation/network/i_http_client.hpp"
#include "foundation/observability/metrics.hpp"
#include "foundation/observability/tracing.hpp"
#include "foundation/scheduler/scheduler.hpp"
#include "foundation/storage/i_storage.hpp"

#include <memory>
#include <string>

namespace lgc {

// Typed ILifecycle component factories for concrete foundation subsystems
// (executor, scheduler, event sink, storage, metrics, trace, HTTP client).
// These live in the app layer because they intentionally couple to specific
// subsystems; the foundation Lifecycle core stays free of those dependencies.

[[nodiscard]] std::shared_ptr<ILifecycle> makeLifecycleComponent(
    std::string name,
    std::shared_ptr<IExecutor> executor);

[[nodiscard]] std::shared_ptr<ILifecycle> makeLifecycleComponent(
    std::string name,
    std::shared_ptr<ITaskScheduler> scheduler);

[[nodiscard]] std::shared_ptr<ILifecycle> makeLifecycleComponent(
    std::string name,
    std::shared_ptr<IEventSink> eventSink);

[[nodiscard]] std::shared_ptr<ILifecycle> makeLifecycleComponent(
    std::string name,
    std::shared_ptr<IStorage> storage);

[[nodiscard]] std::shared_ptr<ILifecycle> makeLifecycleComponent(
    std::string name,
    std::shared_ptr<IMetricRecorder> metrics);

[[nodiscard]] std::shared_ptr<ILifecycle> makeLifecycleComponent(
    std::string name,
    std::shared_ptr<ITraceSink> traceSink);

[[nodiscard]] std::shared_ptr<ILifecycle> makeLifecycleComponent(
    std::string name,
    std::shared_ptr<IHttpClient> httpClient);

} // namespace lgc
