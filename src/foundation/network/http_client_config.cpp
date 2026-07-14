#include "foundation/network/http_client_types.hpp"

#include "foundation/network/http_client_common.hh"

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lc {
using namespace http_client_detail;

std::string_view httpMethodName(HttpMethod method) noexcept
{
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Head:
        return "HEAD";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Delete:
        return "DELETE";
    case HttpMethod::Patch:
        return "PATCH";
    case HttpMethod::Options:
        return "OPTIONS";
    }
    return "GET";
}

bool httpMethodAllowsBody(HttpMethod method) noexcept
{
    return method == HttpMethod::Post
        || method == HttpMethod::Put
        || method == HttpMethod::Delete
        || method == HttpMethod::Patch;
}

bool httpHeaderNameEquals(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
        return false;

    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        const auto lowerLeft = left >= 'A' && left <= 'Z'
            ? static_cast<unsigned char>(left - 'A' + 'a')
            : left;
        const auto lowerRight = right >= 'A' && right <= 'Z'
            ? static_cast<unsigned char>(right - 'A' + 'a')
            : right;
        if (lowerLeft != lowerRight)
            return false;
    }

    return true;
}

HttpRequest HttpRequest::get(std::string pathAndQuery)
{
    HttpRequest request;
    request.method_ = HttpMethod::Get;
    request.pathAndQuery_ = std::move(pathAndQuery);
    return request;
}

HttpRequest HttpRequest::post(std::string pathAndQuery, std::string body, std::string contentType)
{
    HttpRequest request;
    request.method_ = HttpMethod::Post;
    request.pathAndQuery_ = std::move(pathAndQuery);
    request.body_ = std::move(body);
    request.contentType_ = std::move(contentType);
    return request;
}

void HttpRequest::setHeader(std::string name, std::string value)
{
    for (auto& header : headers_) {
        if (httpHeaderNameEquals(header.first, name)) {
            header.second = std::move(value);
            return;
        }
    }
    headers_.emplace_back(std::move(name), std::move(value));
}

std::optional<std::string_view> HttpRequest::header(std::string_view name) const noexcept
{
    for (const auto& header : headers_) {
        if (httpHeaderNameEquals(header.first, name))
            return std::string_view(header.second);
    }
    return std::nullopt;
}

bool HttpResponse::isInformational() const noexcept
{
    return statusCode_ >= 100 && statusCode_ < 200;
}

bool HttpResponse::isSuccessful() const noexcept
{
    return statusCode_ >= 200 && statusCode_ < 300;
}

bool HttpResponse::isRedirection() const noexcept
{
    return statusCode_ >= 300 && statusCode_ < 400;
}

bool HttpResponse::isClientError() const noexcept
{
    return statusCode_ >= 400 && statusCode_ < 500;
}

bool HttpResponse::isServerError() const noexcept
{
    return statusCode_ >= 500 && statusCode_ < 600;
}

std::optional<std::string_view> HttpResponse::header(std::string_view name) const noexcept
{
    for (const auto& header : headers_) {
        if (httpHeaderNameEquals(header.first, name))
            return std::string_view(header.second);
    }
    return std::nullopt;
}

Status HttpStreamOptions::validate() const
{
    if (chunkBytes_ > 16 * 1024 * 1024)
        return Status::outOfRange("HTTP stream chunk bytes is too large");
    return Status::ok();
}

Status HttpRetryPolicy::validate() const
{
    if (maxRetries_ > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - 1))
        return Status::outOfRange("HTTP retry max retries is too large");
    if (delay_ < std::chrono::milliseconds::zero())
        return Status::invalidArgument("HTTP retry delay cannot be negative");
    for (const int code : statusCodes_) {
        if (code < 100 || code > 599)
            return Status::invalidArgument("HTTP retry status code must be in [100, 599]");
    }
    return retryPolicyFromHttp(*this).validate();
}

Status HttpRequestOptions::validate() const
{
    if (timeout_.has_value() && *timeout_ <= Clock::Duration::zero())
        return Status::invalidArgument("HTTP request timeout must be positive");
    if (retryPolicy_.has_value()) {
        if (auto status = retryPolicy_->validate(); !status.isOk())
            return status;
    }
    return Status::ok();
}

Status HttpLogOptions::validate() const
{
    if (logBodies_ && maxBodyBytes_ == 0)
        return Status::invalidArgument("HTTP log max body bytes must be greater than zero when body logging is enabled");
    return Status::ok();
}

bool HttpProxyConfig::isEnabled() const noexcept
{
    return !host_.empty()
        || port_ != 0
        || auth_ != HttpProxyAuth::None
        || !username_.empty()
        || !password_.empty()
        || !bearerToken_.empty();
}

Status HttpProxyConfig::validate() const
{
    if (!isEnabled())
        return Status::ok();
    if (host_.empty())
        return Status::invalidArgument("HTTP proxy host cannot be empty when proxy is enabled");
    if (port_ == 0)
        return Status::invalidArgument("HTTP proxy port cannot be zero when proxy is enabled");
    if (host_.size() > 253)
        return Status::invalidArgument("HTTP proxy host is too long");
    if (containsHeaderInjection(host_) || containsCtl(host_))
        return Status::invalidArgument("HTTP proxy host contains control characters");

    switch (auth_) {
    case HttpProxyAuth::None:
        if (!username_.empty() || !password_.empty() || !bearerToken_.empty())
            return Status::invalidArgument("HTTP proxy credentials require an auth mode");
        break;
    case HttpProxyAuth::Basic:
        if (username_.empty())
            return Status::invalidArgument("HTTP proxy basic auth username cannot be empty");
        if (!bearerToken_.empty())
            return Status::invalidArgument("HTTP proxy bearer token must be empty for basic auth");
        if (username_.size() > 1024 || password_.size() > 4096)
            return Status::invalidArgument("HTTP proxy basic credentials are too large");
        if (containsHeaderInjection(username_) || containsHeaderInjection(password_))
            return Status::invalidArgument("HTTP proxy basic credentials contain CR/LF");
        break;
    case HttpProxyAuth::Bearer:
        if (bearerToken_.empty())
            return Status::invalidArgument("HTTP proxy bearer token cannot be empty");
        if (!username_.empty() || !password_.empty())
            return Status::invalidArgument("HTTP proxy basic credentials must be empty for bearer auth");
        if (bearerToken_.size() > 4096)
            return Status::invalidArgument("HTTP proxy bearer token is too large");
        if (containsHeaderInjection(bearerToken_))
            return Status::invalidArgument("HTTP proxy bearer token contains CR/LF");
        break;
    }
    return Status::ok();
}

Status HttpRequestLimits::validate() const
{
    if (maxHeaderCount_ > 8192)
        return Status::outOfRange("HTTP request max header count is too large");
    if (maxHeaderNameBytes_ > 16 * 1024)
        return Status::outOfRange("HTTP request max header name bytes is too large");
    if (maxHeaderValueBytes_ > 1024 * 1024)
        return Status::outOfRange("HTTP request max header value bytes is too large");
    if (maxContentTypeBytes_ > 16 * 1024)
        return Status::outOfRange("HTTP request max content type bytes is too large");
    if (maxPathAndQueryBytes_ > 1024 * 1024)
        return Status::outOfRange("HTTP request max path bytes is too large");
    return Status::ok();
}

Status HttpClientConfig::validate() const
{
    if (host_.empty())
        return Status::invalidArgument("HTTP client host cannot be empty");
    if (host_.size() > 253)
        return Status::invalidArgument("HTTP client host is too long");
    if (host_.find('\r') != std::string::npos || host_.find('\n') != std::string::npos)
        return Status::invalidArgument("HTTP client host contains CR/LF");
    if (port_ == 0)
        return Status::invalidArgument("HTTP client port cannot be zero");
    if (connectTimeout_ < std::chrono::milliseconds::zero())
        return Status::invalidArgument("HTTP connect timeout cannot be negative");
    if (readTimeout_ < std::chrono::milliseconds::zero())
        return Status::invalidArgument("HTTP read timeout cannot be negative");
    if (writeTimeout_ < std::chrono::milliseconds::zero())
        return Status::invalidArgument("HTTP write timeout cannot be negative");
    if (closeTimeout_ <= std::chrono::milliseconds::zero())
        return Status::invalidArgument("HTTP close timeout must be positive");
    if (tlsOptions_.caBundleFile_.find('\r') != std::string::npos
        || tlsOptions_.caBundleFile_.find('\n') != std::string::npos)
        return Status::invalidArgument("HTTP TLS CA bundle path contains CR/LF");
    if (tlsOptions_.caPath_.find('\r') != std::string::npos
        || tlsOptions_.caPath_.find('\n') != std::string::npos)
        return Status::invalidArgument("HTTP TLS CA path contains CR/LF");
    if (userAgent_.find('\r') != std::string::npos || userAgent_.find('\n') != std::string::npos)
        return Status::invalidArgument("HTTP user agent contains CR/LF");
    if (userAgent_.size() > 1024)
        return Status::invalidArgument("HTTP user agent is too long");
    if (keepAlive_ && connectionPoolSize_ == 0)
        return Status::invalidArgument("HTTP connection pool size must be positive when keep-alive is enabled");
    if (connectionPoolSize_ > 256)
        return Status::outOfRange("HTTP connection pool size is too large");
    if (auto status = retryPolicy_.validate(); !status.isOk())
        return status;
    if (auto status = logOptions_.validate(); !status.isOk())
        return status;
    if (auto status = proxy_.validate(); !status.isOk())
        return status;
    if (auto status = requestLimits_.validate(); !status.isOk())
        return status;
    return Status::ok();
}

Result<HttpClientConfig> HttpClientConfig::fromOrigin(std::string_view input) noexcept
{
    std::string_view rest = trimView(input);
    if (rest.empty())
        return Status::invalidArgument("HTTP origin cannot be empty");

    bool tls = false;
    if (rest.starts_with("https://")) {
        tls = true;
        rest.remove_prefix(8);
    } else if (rest.starts_with("http://")) {
        tls = false;
        rest.remove_prefix(7);
    }

    const std::size_t slash = rest.find('/');
    if (slash != std::string_view::npos)
        rest = rest.substr(0, slash);

    rest = trimView(rest);
    if (rest.empty())
        return Status::invalidArgument("HTTP origin is missing host");

    std::uint16_t port = static_cast<std::uint16_t>(tls ? 443 : 80);
    std::string host;

    if (!rest.empty() && rest.front() == '[') {
        const std::size_t closeBracket = rest.find(']');
        if (closeBracket == std::string_view::npos || closeBracket < 2)
            return Status::invalidArgument("HTTP origin has invalid IPv6 bracket");
        host = std::string(rest.substr(1, closeBracket - 1));
        if (closeBracket + 1 < rest.size()) {
            if (rest[closeBracket + 1] != ':')
                return Status::invalidArgument("HTTP origin has invalid IPv6 suffix");
            const std::string_view portPart = rest.substr(closeBracket + 2);
            if (portPart.empty())
                return Status::invalidArgument("HTTP origin is missing port");
            unsigned parsedPort = 0;
            const auto* begin = portPart.data();
            const auto* end = begin + portPart.size();
            const auto result = std::from_chars(begin, end, parsedPort);
            if (result.ec != std::errc {} || result.ptr != end || parsedPort > 65535)
                return Status::invalidArgument("HTTP origin has invalid port");
            port = static_cast<std::uint16_t>(parsedPort);
        }
    } else {
        const std::size_t colon = rest.rfind(':');
        if (colon != std::string_view::npos && colon > 0) {
            const std::string_view portPart = rest.substr(colon + 1);
            bool digits = !portPart.empty();
            for (char c : portPart) {
                if (c < '0' || c > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                host = std::string(rest.substr(0, colon));
                unsigned parsedPort = 0;
                const auto* begin = portPart.data();
                const auto* end = begin + portPart.size();
                const auto result = std::from_chars(begin, end, parsedPort);
                if (result.ec != std::errc {} || result.ptr != end || parsedPort > 65535)
                    return Status::invalidArgument("HTTP origin has invalid port");
                port = static_cast<std::uint16_t>(parsedPort);
            } else {
                host = std::string(rest);
            }
        } else {
            host = std::string(rest);
        }
    }

    if (host.empty())
        return Status::invalidArgument("HTTP origin host cannot be empty");

    HttpClientConfig out;
    out.host_ = std::move(host);
    out.port_ = port;
    out.useTls_ = tls;
    return out;
}

} // namespace lc
