#pragma once

#include "foundation/network/http_client_types.hpp"

#include <memory>

namespace lc {

class IAuthorizationProvider;

/// Minimal HTTP(S) client abstraction for cloud LLM APIs and local services.
///
/// The synchronous and asynchronous APIs share the same request/response/error model:
/// `Result<HttpResponse>`. Transport failures, timeouts, queue pressure, and lifecycle errors are
/// returned as `Status`; HTTP 4xx/5xx responses are valid `HttpResponse` values so callers can decide
/// how to handle provider-specific error bodies.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    [[nodiscard]] virtual HttpResult send(
        HttpRequest request,
        HttpRequestOptions options)
        = 0;

    [[nodiscard]] virtual Status sendAsync(
        HttpRequest request,
        HttpRequestOptions options,
        HttpCallback callback)
        = 0;

    /// Streams response body chunks to `callback` and returns response status/headers when the
    /// stream ends. The returned `HttpResponse::body_` is intentionally empty. Streaming requests
    /// use a dedicated long-lived connection and are not transparently retried after bytes arrive.
    [[nodiscard]] virtual HttpResult sendStreaming(
        HttpRequest request,
        HttpRequestOptions requestOptions,
        HttpBodyChunkCallback callback,
        HttpStreamOptions options = {})
        = 0;

    /// Parses a `text/event-stream` response and invokes `callback` for each SSE event. SSE uses
    /// the same dedicated streaming connection semantics as `sendStreaming`.
    [[nodiscard]] virtual HttpResult sendSse(
        HttpRequest request,
        HttpRequestOptions requestOptions,
        ServerSentEventCallback callback,
        HttpStreamOptions options = {})
        = 0;

    [[nodiscard]] virtual std::shared_ptr<IAuthorizationProvider> authorizationProvider() const = 0;

    [[nodiscard]] virtual Status close() = 0;
    
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

} // namespace lc
