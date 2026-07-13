#pragma once

#include "foundation/blob/blob_store.hpp"
#include "foundation/cache/cache.hpp"
#include "foundation/event/i_event_sink.hpp"
#include "foundation/executor/i_executor.hpp"
#include "foundation/lifecycle/lifecycle.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/network/i_http_client.hpp"
#include "foundation/observability/metrics.hpp"
#include "foundation/observability/tracing.hpp"
#include "foundation/scheduler/scheduler.hpp"
#include "foundation/secrets/secret_provider.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "foundation/storage/i_storage.hpp"

#include <memory>
#include <mutex>

namespace lc {

struct RuntimeServiceRequirements {
    bool logger_ { true };
    bool storage_ { true };
    bool blobStore_ { false };
    bool cache_ { false };
    bool executor_ { true };
    bool scheduler_ { true };
    bool httpClient_ { false };
    bool secrets_ { true };
    bool eventSink_ { false };
    bool metrics_ { false };
    bool traceSink_ { false };

    [[nodiscard]] static RuntimeServiceRequirements core() noexcept;
    [[nodiscard]] static RuntimeServiceRequirements all() noexcept;
};

struct RuntimeServices {
    std::shared_ptr<ILogger> logger_;
    std::shared_ptr<IStorage> storage_;
    std::shared_ptr<IBlobStore> blobStore_;
    std::shared_ptr<ICache> cache_;
    std::shared_ptr<IExecutor> executor_;
    std::shared_ptr<ITaskScheduler> scheduler_;
    std::shared_ptr<IHttpClient> httpClient_;
    std::shared_ptr<ISecrets> secrets_;
    std::shared_ptr<IEventSink> eventSink_;
    std::shared_ptr<IMetricRecorder> metrics_;
    std::shared_ptr<ITraceSink> traceSink_;

    [[nodiscard]] Status validate(
        RuntimeServiceRequirements requirements = RuntimeServiceRequirements::core()) const;
    [[nodiscard]] Result<std::shared_ptr<Lifecycle>> createLifecycle() const;
};

[[nodiscard]] RuntimeServices defaultRuntimeServices();

/// Runtime-facing service owner. Prefer this container plus `Lifecycle` for runtime integration;
/// low-level `threading` / `timer` objects should be owned behind executors and schedulers.
struct RuntimeContainerOptions {
    RuntimeServiceRequirements requirements_ { RuntimeServiceRequirements::core() };
    CloseOptions closeOptions_;
};

class RuntimeContainer final {
public:
    explicit RuntimeContainer(
        RuntimeServices services,
        RuntimeContainerOptions options = {});
    ~RuntimeContainer();

    RuntimeContainer(const RuntimeContainer&) = delete;
    RuntimeContainer& operator=(const RuntimeContainer&) = delete;
    RuntimeContainer(RuntimeContainer&&) = delete;
    RuntimeContainer& operator=(RuntimeContainer&&) = delete;

    [[nodiscard]] const RuntimeServices& services() const noexcept;
    [[nodiscard]] RuntimeServices& services() noexcept;

    [[nodiscard]] Status validate() const;
    [[nodiscard]] Status start();
    [[nodiscard]] Status waitIdle(Clock::Duration timeout);
    [[nodiscard]] Status close(CloseOptions options = {});
    [[nodiscard]] bool isClosed() const noexcept;
    [[nodiscard]] std::shared_ptr<Lifecycle> lifecycle() const;

private:
    [[nodiscard]] Status ensureLifecycleLocked();

    mutable std::mutex mutex_;
    RuntimeServices services_;
    RuntimeContainerOptions options_;
    std::shared_ptr<Lifecycle> lifecycle_;
    bool closed_ { false };
};

[[nodiscard]] Result<std::shared_ptr<RuntimeContainer>> createRuntimeContainer(
    RuntimeServices services,
    RuntimeContainerOptions options = {});

} // namespace lc
