#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/network/i_authorization_provider.hpp"
#include "foundation/network/i_http_client.hpp"

#include <memory>
#include <mutex>

namespace lgc {

/// Outbound HTTP/HTTPS client: cpp-httplib + connection pool for buffered requests, dedicated
/// streaming connections for body/SSE streams, serial async queue, and request authorization gate.
class HttpClient final : public IHttpClient {
public:
    explicit HttpClient(
        HttpClientConfig config,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    /// Injects a custom request authorization provider. If null, builds a `NoAuthorizationProvider`.
    /// Prefer this over a runtime setter so the worker always sees a stable provider + wake hook.
    HttpClient(
        HttpClientConfig config,
        std::shared_ptr<IAuthorizationProvider> authorizationProvider,
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    ~HttpClient() override;

    [[nodiscard]] HttpResult send(
        HttpRequest request,
        HttpRequestOptions options) override;

    [[nodiscard]] Status sendAsync(
        HttpRequest request,
        HttpRequestOptions options,
        HttpCallback callback) override;

    [[nodiscard]] HttpResult sendStreaming(
        HttpRequest request,
        HttpRequestOptions requestOptions,
        HttpBodyChunkCallback callback,
        HttpStreamOptions options = {}) override;

    [[nodiscard]] HttpResult sendSse(
        HttpRequest request,
        HttpRequestOptions requestOptions,
        ServerSentEventCallback callback,
        HttpStreamOptions options = {}) override;

    [[nodiscard]] std::shared_ptr<IAuthorizationProvider> authorizationProvider() const override;

    [[nodiscard]] Status close() override;
    
    [[nodiscard]] bool isClosed() const noexcept override;

public:
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

private:
    class Impl;
    mutable std::mutex mutex_;
    std::shared_ptr<Impl> impl_;
};

} // namespace lgc
