#pragma once

#include "foundation/network/http_client_types.hpp"
#include "foundation/retry/retry_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace lc::http_client_detail {

inline std::string_view trimView(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

inline bool hasAuthorizationHeader(const HttpRequest& request)
{
    for (const auto& h : request.headers_) {
        if (httpHeaderNameEquals(h.first, "authorization"))
            return true;
    }
    return false;
}

[[nodiscard]] inline bool isValidHeaderName(std::string_view name) noexcept
{
    if (name.empty())
        return false;

    for (const unsigned char ch : name) {
        const bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        const bool digit = ch >= '0' && ch <= '9';
        switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            break;
        default:
            if (!alpha && !digit)
                return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool containsHeaderInjection(std::string_view value) noexcept
{
    return value.find('\r') != std::string_view::npos
        || value.find('\n') != std::string_view::npos;
}

[[nodiscard]] inline Status checkMaxBytes(std::string_view value, std::size_t maxBytes, std::string_view name)
{
    if (maxBytes > 0 && value.size() > maxBytes) {
        return Status::resourceExhausted(
            std::string(name) + " exceeds configured byte limit");
    }
    return Status::ok();
}

[[nodiscard]] inline bool containsCtl(std::string_view value) noexcept
{
    for (const unsigned char c : value) {
        if (c < 0x20 || c == 0x7f)
            return true;
    }
    return false;
}

[[nodiscard]] inline Status validateRequest(const HttpRequest& request, const HttpRequestLimits& limits)
{
    if (auto status = checkMaxBytes(
            request.pathAndQuery_,
            limits.maxPathAndQueryBytes_,
            "HTTP request path");
        !status.isOk()) {
        return status;
    }
    if (request.pathAndQuery_.find('\r') != std::string::npos
        || request.pathAndQuery_.find('\n') != std::string::npos) {
        return Status::invalidArgument("HTTP request path contains CR/LF");
    }
    if (request.contentType_.has_value()) {
        if (auto status = checkMaxBytes(
                *request.contentType_,
                limits.maxContentTypeBytes_,
                "HTTP content type");
            !status.isOk()) {
            return status;
        }
        if (containsHeaderInjection(*request.contentType_))
            return Status::invalidArgument("HTTP content type contains CR/LF");
    }
    if (auto status = checkMaxBytes(
            request.body_,
            limits.maxRequestBodyBytes_,
            "HTTP request body");
        !status.isOk()) {
        return status;
    }
    if (limits.maxHeaderCount_ > 0 && request.headers_.size() > limits.maxHeaderCount_)
        return Status::resourceExhausted("HTTP request header count exceeds configured limit");

    for (const auto& [name, value] : request.headers_) {
        if (auto status = checkMaxBytes(name, limits.maxHeaderNameBytes_, "HTTP header name");
            !status.isOk()) {
            return status;
        }
        if (auto status = checkMaxBytes(value, limits.maxHeaderValueBytes_, "HTTP header value");
            !status.isOk()) {
            return status;
        }
        if (!isValidHeaderName(name))
            return Status::invalidArgument("HTTP header name is invalid");
        if (containsHeaderInjection(value))
            return Status::invalidArgument("HTTP header value contains CR/LF");
    }
    return Status::ok();
}

struct PendingJob {
    HttpRequest request;
    HttpRequestOptions options;
    HttpCallback cb;
};

inline void invokeCallback(HttpCallback cb, HttpResult result) noexcept
{
    if (!cb)
        return;
    try {
        cb(std::move(result));
    } catch (const std::exception&) {
    } catch (...) {
    }
}

[[nodiscard]] inline Status statusForHttpRetry(int statusCode)
{
    if (statusCode == 408)
        return Status::deadlineExceeded("HTTP status 408 is retryable");
    if (statusCode == 429)
        return Status::resourceExhausted("HTTP status 429 is retryable");
    if (statusCode >= 500 && statusCode < 600)
        return Status::unavailable("HTTP server status is retryable: " + std::to_string(statusCode));
    return Status::aborted("HTTP status is retryable: " + std::to_string(statusCode));
}

[[nodiscard]] inline bool circuitFailureStatus(int statusCode) noexcept
{
    return statusCode == 408 || statusCode == 429 || (statusCode >= 500 && statusCode < 600);
}

[[nodiscard]] inline RetryPolicy retryPolicyFromHttp(const HttpRetryPolicy& policy)
{
    const auto attempts = static_cast<std::uint32_t>(policy.maxRetries_ + 1);
    return RetryPolicy::fixed(attempts, policy.delay_);
}

[[nodiscard]] inline bool containsHttpRetryCode(const HttpRetryPolicy& policy, int statusCode) noexcept
{
    return std::find(policy.statusCodes_.begin(), policy.statusCodes_.end(), statusCode)
        != policy.statusCodes_.end();
}

inline std::string normalizePath(std::string path)
{
    if (path.empty())
        return "/";
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    return path;
}

inline std::string formatOrigin(const HttpClientConfig& cfg)
{
    std::ostringstream oss;
    oss << (cfg.useTls_ ? "https://" : "http://");
    if (cfg.host_.find(':') != std::string::npos) {
        oss << '[' << cfg.host_ << ']';
    } else {
        oss << cfg.host_;
    }
    const bool nonDefault = (cfg.useTls_ && cfg.port_ != 443) || (!cfg.useTls_ && cfg.port_ != 80);
    if (nonDefault) {
        oss << ':' << static_cast<unsigned>(cfg.port_);
    }
    return oss.str();
}

} // namespace lc::http_client_detail
