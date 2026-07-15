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

namespace lgc {

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

} // namespace lgc
