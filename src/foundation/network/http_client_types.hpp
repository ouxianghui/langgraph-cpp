#pragma once

#include "foundation/status/result.hpp"
#include "foundation/time/deadline.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lc {

class CircuitBreaker;
class IRateLimiter;
class IMetricRecorder;
class ITraceSink;

enum class TlsMinVersion : std::uint8_t {
    Default = 0,
    Tls12,
    Tls13,
};

struct TlsOptions {
    TlsMinVersion minVersion_ { TlsMinVersion::Default };
    bool verifyPeer_ { true };
    std::string caBundleFile_;
    std::string caPath_;
};

enum class HttpMethod : std::uint8_t {
    Get,
    Head,
    Post,
    Put,
    Delete,
    Patch,
    Options,
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::string_view httpMethodName(HttpMethod method) noexcept;
[[nodiscard]] bool httpMethodAllowsBody(HttpMethod method) noexcept;
[[nodiscard]] bool httpHeaderNameEquals(std::string_view lhs, std::string_view rhs) noexcept;

struct HttpRequest {
    HttpMethod method_ { HttpMethod::Get };
    std::string pathAndQuery_ { "/" };
    HttpHeaders headers_;
    std::string body_;
    std::optional<std::string> contentType_;

    [[nodiscard]] static HttpRequest get(std::string pathAndQuery = "/");
    [[nodiscard]] static HttpRequest post(
        std::string pathAndQuery,
        std::string body,
        std::string contentType = "application/json");

    void setHeader(std::string name, std::string value);
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
};

struct HttpResponse {
    int statusCode_ { 0 };
    std::string reason_;
    HttpHeaders headers_;
    std::string body_;

    [[nodiscard]] bool isInformational() const noexcept;
    [[nodiscard]] bool isSuccessful() const noexcept;
    [[nodiscard]] bool isRedirection() const noexcept;
    [[nodiscard]] bool isClientError() const noexcept;
    [[nodiscard]] bool isServerError() const noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept;
};

struct HttpStreamOptions {
    /// Maximum bytes delivered per chunk. `0` uses the transport default.
    std::size_t chunkBytes_ { 8 * 1024 };

    [[nodiscard]] Status validate() const;
};

using HttpBodyChunkCallback = std::function<Status(std::string_view chunk)>;

struct ServerSentEvent {
    std::string event_;
    std::string data_;
    std::string id_;
    std::optional<std::chrono::milliseconds> retry_;
};

using ServerSentEventCallback = std::function<Status(const ServerSentEvent& event)>;

struct HttpRetryPolicy {
    std::size_t maxRetries_ { 0 };
    std::chrono::milliseconds delay_ { 200 };
    std::vector<int> statusCodes_ { 502, 503, 504 };

    [[nodiscard]] Status validate() const;
};

struct HttpRequestOptions {
    /// Optional whole-request budget. Converted to an absolute monotonic deadline when the request is
    /// accepted, so async queue time counts against the same budget.
    std::optional<Clock::Duration> timeout_;
    Deadline deadline_ { Deadline::none() };
    /// Buffered requests use this retry policy when set; streaming/SSE requests are not
    /// transparently retried after bytes are requested.
    std::optional<HttpRetryPolicy> retryPolicy_;

    [[nodiscard]] Status validate() const;
};

struct HttpLogOptions {
    bool logHeaders_ { true };
    bool logBodies_ { false };
    std::size_t maxBodyBytes_ { 4096 };

    [[nodiscard]] Status validate() const;
};

enum class HttpProxyAuth : std::uint8_t {
    None = 0,
    Basic,
    Bearer,
};

struct HttpProxyConfig {
    std::string host_;
    std::uint16_t port_ { 0 };
    HttpProxyAuth auth_ { HttpProxyAuth::None };
    std::string username_;
    std::string password_;
    std::string bearerToken_;

    [[nodiscard]] bool isEnabled() const noexcept;
    [[nodiscard]] Status validate() const;
};

struct HttpRequestLimits {
    std::size_t maxPathAndQueryBytes_ { 8 * 1024 };
    std::size_t maxHeaderCount_ { 128 };
    std::size_t maxHeaderNameBytes_ { 128 };
    std::size_t maxHeaderValueBytes_ { 16 * 1024 };
    std::size_t maxContentTypeBytes_ { 256 };
    std::size_t maxRequestBodyBytes_ { 16 * 1024 * 1024 };

    [[nodiscard]] Status validate() const;
};

struct HttpClientConfig {
    std::string host_;
    std::uint16_t port_ { 80 };
    bool useTls_ { false };
    TlsOptions tlsOptions_ {};

    std::chrono::milliseconds connectTimeout_ { 30'000 };
    std::chrono::milliseconds readTimeout_ { 30'000 };
    std::chrono::milliseconds writeTimeout_ { 30'000 };
    std::chrono::milliseconds closeTimeout_ { 5'000 };

    HttpRetryPolicy retryPolicy_;
    HttpLogOptions logOptions_;
    std::size_t maxPendingRequests_ { 0 };
    std::size_t maxResponseBodyBytes_ { 0 };
    std::string userAgent_;
    bool keepAlive_ { true };
    std::size_t connectionPoolSize_ { 4 };
    bool followRedirects_ { false };
    HttpProxyConfig proxy_;
    HttpRequestLimits requestLimits_;
    std::shared_ptr<IRateLimiter> rateLimiter_;
    std::shared_ptr<CircuitBreaker> circuitBreaker_;
    std::shared_ptr<IMetricRecorder> metrics_;
    std::shared_ptr<ITraceSink> traceSink_;

    [[nodiscard]] static Result<HttpClientConfig> fromOrigin(std::string_view origin) noexcept;
    [[nodiscard]] Status validate() const;
};

enum class OAuthTokenRefreshReason : std::uint8_t {
    ProactiveRenewWindow,
};

using OAuthTokenRefreshHandler = std::function<void(OAuthTokenRefreshReason reason)>;

struct OAuthCredentials {
    std::string accessToken_;
    std::string refreshToken_;
    std::optional<std::chrono::system_clock::time_point> accessTokenExpiresAt_;
    std::optional<std::chrono::seconds> tokenRenewLeadTime_;
};

struct AuthorizationGate {
    bool paused_ {};
    std::optional<std::chrono::system_clock::time_point> resumeAt_;
};

using HttpResult = Result<HttpResponse>;
using HttpCallback = std::function<void(HttpResult)>;

} // namespace lc
