#include "core/runtime/runtime_container.hpp"
#include "core/runtime/runtime_services.hpp"
#include "foundation/executor/inline_executor.hpp"
#include "foundation/status/status.hpp"
#include "foundation/storage/memory_storage.hpp"

#include <cassert>
#include <chrono>
#include <memory>

int main()
{
    using namespace std::chrono_literals;

    auto services = lgc::defaultRuntimeServices();
    assert(services.validate().isOk());
    assert(services.logger_);
    assert(services.storage_);
    assert(services.blobStore_);
    assert(services.cache_);
    assert(services.executor_);
    assert(services.scheduler_);
    assert(!services.httpClient_);
    assert(services.secrets_);
    assert(services.eventSink_);
    assert(services.metrics_);
    assert(services.traceSink_);

    auto requireHttp = lgc::RuntimeServiceRequirements::core();
    requireHttp.httpClient_ = true;
    assert(services.validate(requireHttp).code() == lgc::StatusCode::FailedPrecondition);

    auto requireAll = lgc::RuntimeServiceRequirements::all();
    assert(services.validate(requireAll).code() == lgc::StatusCode::FailedPrecondition);

    lgc::RuntimeServices custom;
    custom.logger_ = services.logger_;
    custom.storage_ = std::make_shared<lgc::MemoryStorage>();
    custom.executor_ = std::make_shared<lgc::InlineExecutor>();
    custom.scheduler_ = services.scheduler_;
    custom.secrets_ = services.secrets_;
    assert(custom.validate().isOk());

    assert(custom.executor_->close(0ms).isOk());
    assert(custom.validate().code() == lgc::StatusCode::FailedPrecondition);

    lgc::RuntimeServices missing;
    assert(missing.validate().code() == lgc::StatusCode::FailedPrecondition);

    auto lifecycleServices = lgc::defaultRuntimeServices();
    auto lifecycle = lifecycleServices.createLifecycle();
    assert(lifecycle.isOk());
    assert((*lifecycle)->count() == 6);
    assert((*lifecycle)->start().isOk());
    assert((*lifecycle)->close(lgc::CloseOptions { .timeout_ = 1s }).isOk());
    assert(lifecycleServices.validate().code() == lgc::StatusCode::FailedPrecondition);

    auto container = lgc::createRuntimeContainer(lgc::defaultRuntimeServices());
    assert(container.isOk());
    assert((*container)->validate().isOk());
    assert((*container)->start().isOk());
    assert((*container)->waitIdle(1s).isOk());
    assert((*container)->lifecycle());
    assert((*container)->close(lgc::CloseOptions { .timeout_ = 1s }).isOk());
    assert((*container)->isClosed());
    assert((*container)->validate().code() == lgc::StatusCode::FailedPrecondition);

    auto missingContainer = lgc::createRuntimeContainer(lgc::RuntimeServices {});
    assert(missingContainer.status().code() == lgc::StatusCode::FailedPrecondition);

    return 0;
}
