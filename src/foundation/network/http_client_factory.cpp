#include "foundation/network/http_client_factory.hpp"

#include "foundation/network/http_client.hpp"
#include "foundation/redaction/redactor.hpp"

#include <utility>

namespace lgc {

DefaultHttpClientFactory::DefaultHttpClientFactory(HttpClientFactoryOptions options)
    : options_(std::move(options))
{
    if (!options_.logger_)
        options_.logger_ = Logger::defaultLogger();
}

Result<std::shared_ptr<IHttpClient>> DefaultHttpClientFactory::create(
    HttpClientConfig config,
    std::shared_ptr<IAuthorizationProvider> authorizationProvider)
{
    if (!config.rateLimiter_)
        config.rateLimiter_ = options_.rateLimiter_;
    if (!config.circuitBreaker_)
        config.circuitBreaker_ = options_.circuitBreaker_;
    if (!config.metrics_)
        config.metrics_ = options_.metrics_;
    if (!config.traceSink_)
        config.traceSink_ = options_.traceSink_;
    if (auto status = config.validate(); !status.isOk())
        return status;

    auto logger = options_.logger_ ? options_.logger_ : Logger::defaultLogger();
    if (options_.redactLogs_)
        logger = std::make_shared<RedactionLogger>(std::move(logger));

    std::shared_ptr<IHttpClient> client = std::make_shared<HttpClient>(
        std::move(config),
        std::move(authorizationProvider),
        std::move(logger));
    return client;
}

std::shared_ptr<IHttpClientFactory> defaultHttpClientFactory(HttpClientFactoryOptions options)
{
    return std::make_shared<DefaultHttpClientFactory>(std::move(options));
}

} // namespace lgc
