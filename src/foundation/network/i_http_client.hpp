#pragma once

#include "foundation/network/http_client_types.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace lc {

class IOAuth2TokenProvider;

/// Abstract asynchronous HTTP(S) client with optional bearer token lifecycle.
///
/// `request` returns immediately; `HttpAsyncCallback` receives `std::shared_ptr<HttpCallResult>`
/// on an internal worker thread after the transport call completes (do not block inside the callback).
/// Exceptions thrown from the callback are caught and discarded so the worker thread keeps running.
///
/// `requestSpec` must be non-null; if null, the completion callback receives `HttpClientErr::Unknown`
/// synchronously. The client keeps the `shared_ptr` until the request finishes so the payload stays
/// valid while queued.
///
/// When `HttpClientConfig::maxPendingRequests_` is non-zero and the queue is full, the completion
/// callback receives `HttpClientErr::QueueFull` synchronously before returning from `request`.
///
/// When `monitorLastReadIdle_` and/or `monitorLastWriteIdle_` are set, pooled connections may be
/// closed when idle budgets are exceeded; failures may map to `HttpClientErr::Timeout` where
/// distinguishable.
///
/// For HTTPS, `HttpClientConfig::tlsParams_` carries TLS minimum version, peer verification, and
/// optional CA bundle / CApath (shared with `WebsocketClientConfig::tlsParams_`).
///
/// OAuth2 tokens and hooks live on **`oauth2TokenProvider()`** (`apply`, `onRenew`, `gate`, etc.).
/// Inject a custom provider via **`HttpClient(config, shared_ptr<IOAuth2TokenProvider>)`** (no runtime
/// setter: swapping providers while the worker is running is unsafe without draining the queue).
/// When the renew window is reached, dispatch pauses until updated credentials clear deferral.
class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    virtual void request(std::shared_ptr<HttpRequestSpec> requestSpec, HttpAsyncCallback callback) = 0;

    /// Shared credential supplier used for Bearer injection and proactive-renew handling. Never null for a
    /// live `HttpClient`; may be null after `shutdown`.
    [[nodiscard]] virtual std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider() const = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace lc
