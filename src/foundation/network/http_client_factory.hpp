#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/network/http_client_types.hpp"

#include <memory>

namespace lc {

class IHttpClient;
class IAuthorizationProvider;
class Redactor;

struct HttpClientFactoryOptions {
    std::shared_ptr<ILogger> logger_ { Logger::defaultLogger() };
    std::shared_ptr<IRateLimiter> rateLimiter_;
    std::shared_ptr<CircuitBreaker> circuitBreaker_;
    std::shared_ptr<IMetricRecorder> metrics_;
    std::shared_ptr<ITraceSink> traceSink_;
    bool redactLogs_ { true };
};

class IHttpClientFactory {
public:
    virtual ~IHttpClientFactory() = default;

    [[nodiscard]] virtual Result<std::shared_ptr<IHttpClient>> create(
        HttpClientConfig config,
        std::shared_ptr<IAuthorizationProvider> authorizationProvider)
        = 0;
};

class DefaultHttpClientFactory final : public IHttpClientFactory {
public:
    explicit DefaultHttpClientFactory(HttpClientFactoryOptions options = {});

    [[nodiscard]] Result<std::shared_ptr<IHttpClient>> create(
        HttpClientConfig config,
        std::shared_ptr<IAuthorizationProvider> authorizationProvider) override;

private:
    HttpClientFactoryOptions options_;
};

[[nodiscard]] std::shared_ptr<IHttpClientFactory> defaultHttpClientFactory(
    HttpClientFactoryOptions options = {});

} // namespace lc
