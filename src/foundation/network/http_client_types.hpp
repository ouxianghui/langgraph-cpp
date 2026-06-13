#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lc {

/// Minimum TLS protocol version when using HTTPS (`HttpClientConfig::tlsParams_`).
enum class TlsMinVersion : std::uint8_t {
    /// Do not set `SSL_CTX_set_min_proto_version` (follow OpenSSL / build defaults).
    Default = 0,
    Tls12,
    Tls13,
};

/// TLS client knobs for outbound HTTP/WebSocket clients (`HttpClient`, `WebsocketClient`, etc.).
struct TlsClientParams {
    TlsMinVersion tlsMinVersion_ { TlsMinVersion::Default };
    bool tlsVerifyPeer_ { true };
    std::string tlsCaBundleFile_;
    std::string tlsCaPath_;
};

enum class HttpClientErr : std::uint8_t {
    Ok = 0,
    PoolAcquireTimeout,
    Transport,
    Protocol,
    EmptyResponse,
    /// Connection closed by `ConnectionMonitor` idle rules (`monitorLastReadIdle_` /
    /// `monitorLastWriteIdle_`) or matching transport failure while monitoring is enabled.
    Timeout,
    Unknown,
    Stopped,
    /// `IHttpClient::request` rejected: pending queue is at `maxPendingRequests_`.
    QueueFull,
    /// Response body exceeds `maxResponseBodyBytes_` (Content-Length or post-read check).
    PayloadTooLarge,
};

/// Tunables for a single remote origin (`host` + `port` + TLS). Requests are executed via libcpr on
/// the client's worker thread (see `HttpClientConfig::pool*` for reserved tuning fields).
struct HttpClientConfig {
    std::string host_;
    std::uint16_t port_ { 80 };
    bool tls_ { false };

    /// Used when `tls_` is true (same shape as `WebsocketClientConfig::tlsParams_`).
    TlsClientParams tlsParams_ {};

    std::int64_t poolMaxConnections_ { 32 };
    std::chrono::microseconds poolIdleTtl_ { std::chrono::minutes(5) };
    /// Per-acquire wait when the pool is saturated (`0` = wait indefinitely).
    std::chrono::microseconds poolAcquireTimeout_ { 0 };

    /// When set, enables transport-level retries on selected HTTP statuses.
    std::optional<std::int64_t> retryMaxAttempts_;
    std::chrono::microseconds retryDelay_ { std::chrono::milliseconds(200) };
    std::vector<int> retryHttpCodes_ { 502, 503, 504 };

    /// Max queued `request` jobs waiting for the worker (`0` = unlimited).
    std::size_t maxPendingRequests_ { 0 };

    /// Max accepted response body size in bytes (`0` = unlimited). When non-zero, rejects bodies
    /// larger than this via `Content-Length` when present, and checks size after read otherwise.
    std::size_t maxResponseBodyBytes_ { 0 };

    /// Default `User-Agent` for outbound requests. A `User-Agent` in `HttpRequestSpec::headers_`
    /// overrides this value for that request.
    std::string userAgent_;

    /// When non-zero, enables read/write idle monitoring on pooled connections (implementation-defined).
    std::chrono::microseconds monitorLastReadIdle_ { 0 };
    std::chrono::microseconds monitorLastWriteIdle_ { 0 };

    /// Minimal `http(s)://host[:port][/…]` parser (non-default ports, optional `[ipv6]:port`).
    static HttpClientConfig parseOrigin(std::string_view origin);
};

/// Why `IHttpClient` / `OAuth2TokenProvider` suggests refreshing tokens (credential-provider style).
enum class OAuth2TokenRefreshReason : std::uint8_t {
    /// `now >= accessTokenExpiresAt - renewLead` with a non-empty access token (proactive renew window).
    ProactiveRenewWindow,
};

using OAuth2TokenRefreshHandler = std::function<void(OAuth2TokenRefreshReason reason)>;

/// OAuth 2.0–style bearer credentials (`Authorization: Bearer …`) plus optional refresh token and
/// expiry-driven pause semantics. Passed into `IOAuth2TokenProvider::apply`; the default
/// `OAuth2TokenProvider` stores a full snapshot (`OAuth2Credentials`) plus a resolved renew-lead
/// duration (see `OAuth2TokenProvider`).
struct OAuth2Credentials {
    std::string accessToken_;
    std::string refreshToken_;
    /// Absolute expiry of `accessToken_`. When unset, the provider does not pause outgoing `request`
    /// calls based on wall-clock expiry.
    std::optional<std::chrono::system_clock::time_point> accessTokenExpiresAt_;
    /// Optional renew-window override for `OAuth2TokenProvider::apply`: when set, replaces the
    /// provider’s resolved renew-lead duration; when unset, the previous resolved lead (initially
    /// `OAuth2TokenProvider::kDefaultRenewLead`) is kept.
    std::optional<std::chrono::seconds> tokenRenewLeadTime_;
};

/// Snapshot from `IOAuth2TokenProvider::gate()` for worker scheduling (pause queue vs wake time).
struct OAuth2TokenGate {
    bool pause {};
    std::optional<std::chrono::system_clock::time_point> wakeAt;
};

struct HttpRequestSpec {
    std::string method_ { "GET" };
    /// Path and query only, starting with `/` (e.g. `/v1/ping?x=1`).
    std::string pathAndQuery_ { "/" };
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    /// When `body_` is non-empty and set, used as the `Content-Type` request header.
    std::optional<std::string> contentType_;
};

struct HttpResponseData {
    int statusCode_ { 0 };
    std::string statusText_;
    std::unordered_multimap<std::string, std::string> headers_;
    std::string body_;
};

struct HttpCallResult {
    HttpClientErr error_ { HttpClientErr::Ok };
    std::string errorMessage_;
    /// Populated after a successful HTTP round-trip with headers (and usually body); null on many
    /// transport/protocol errors before response metadata exists.
    std::shared_ptr<HttpResponseData> response_;

    [[nodiscard]] bool isOk() const noexcept
    {
        return error_ == HttpClientErr::Ok;
    }
    explicit operator bool() const noexcept { return isOk(); }
};

using HttpAsyncCallback = std::function<void(std::shared_ptr<HttpCallResult>)>;

} // namespace lc
