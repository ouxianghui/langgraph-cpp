#include "foundation/network/http_client.hpp"

#include "foundation/logging/logger.hpp"
#include "foundation/network/i_oauth2_token_provider.hpp"
#include "foundation/network/oauth2_token_provider.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace lc {
namespace {

std::string_view trimSv(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

bool asciiLowerContains(std::string_view hay, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > hay.size())
        return false;
    for (std::size_t i = 0; i <= hay.size() - needle.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            char ha = hay[i + j];
            char nb = needle[j];
            if (ha >= 'A' && ha <= 'Z')
                ha = static_cast<char>(ha - 'A' + 'a');
            if (nb >= 'A' && nb <= 'Z')
                nb = static_cast<char>(nb - 'A' + 'a');
            if (ha != nb) {
                ok = false;
                break;
            }
        }
        if (ok)
            return true;
    }
    return false;
}

/// Heuristic for curl errors that did not map to `ErrorCode::OPERATION_TIMEDOUT` but read as timeouts.
bool isTimeoutLikeMessage(std::string_view msg)
{
    return asciiLowerContains(msg, "timed out") || asciiLowerContains(msg, "timeout") || asciiLowerContains(msg, "operation timed out") || asciiLowerContains(msg, "resource temporarily unavailable");
}

cpr::SslOptions buildSslOptions(const TlsClientParams& p)
{
    cpr::SslOptions opts = cpr::Ssl(cpr::ssl::VerifyPeer { p.tlsVerifyPeer_ },
        cpr::ssl::VerifyHost { p.tlsVerifyPeer_ });
    switch (p.tlsMinVersion_) {
    case TlsMinVersion::Tls12:
        opts.SetOption(cpr::ssl::TLSv1_2 {});
        break;
    case TlsMinVersion::Tls13:
        opts.SetOption(cpr::ssl::TLSv1_3 {});
        break;
    default:
        break;
    }
    if (!p.tlsCaBundleFile_.empty()) {
        opts.SetOption(
            cpr::ssl::CaInfo { std::filesystem::path(p.tlsCaBundleFile_) });
    }
    if (!p.tlsCaPath_.empty()) {
        opts.SetOption(cpr::ssl::CaPath { std::filesystem::path(p.tlsCaPath_) });
    }
    return opts;
}

bool icmpAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        unsigned char la = ca >= 'A' && ca <= 'Z' ? static_cast<unsigned char>(ca - 'A' + 'a') : ca;
        unsigned char lb = cb >= 'A' && cb <= 'Z' ? static_cast<unsigned char>(cb - 'A' + 'a') : cb;
        if (la != lb)
            return false;
    }
    return true;
}

bool requestHasAuthorizationHeader(const HttpRequestSpec& spec)
{
    for (const auto& h : spec.headers_) {
        if (icmpAscii(h.first, "authorization"))
            return true;
    }
    return false;
}

[[nodiscard]] std::string formatHeadersForLog(const cpr::Header& hdr)
{
    std::ostringstream oss;
    for (const auto& kv : hdr) {
        oss << kv.first << ": " << kv.second << '\n';
    }
    return oss.str();
}

struct PendingJob {
    std::shared_ptr<HttpRequestSpec> spec;
    HttpAsyncCallback cb;
};

void safeCallback(HttpAsyncCallback cb,
    std::shared_ptr<HttpCallResult> res) noexcept
{
    if (!cb || !res)
        return;
    try {
        cb(std::move(res));
    } catch (const std::exception&) {
    } catch (...) {
    }
}

std::optional<std::size_t> parseContentLengthHeader(const cpr::Header& hdr)
{
    for (const auto& kv : hdr) {
        std::string key;
        key.reserve(kv.first.size());
        for (unsigned char c : kv.first) {
            key.push_back(static_cast<char>(std::tolower(c)));
        }
        if (key != "content-length")
            continue;
        const std::string_view val = trimSv(kv.second);
        if (val.empty())
            return std::nullopt;
        try {
            return static_cast<std::size_t>(
                std::stoull(std::string(val.data(), val.size())));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string normalizePath(std::string path)
{
    if (path.empty())
        return "/";
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    return path;
}

std::string buildOriginBase(const HttpClientConfig& cfg)
{
    std::ostringstream oss;
    oss << (cfg.tls_ ? "https://" : "http://");
    if (cfg.host_.find(':') != std::string::npos) {
        oss << '[' << cfg.host_ << ']';
    } else {
        oss << cfg.host_;
    }
    const bool nonDefault = (cfg.tls_ && cfg.port_ != 443) || (!cfg.tls_ && cfg.port_ != 80);
    if (nonDefault) {
        oss << ':' << static_cast<unsigned>(cfg.port_);
    }
    return oss.str();
}

std::string toUpperMethod(std::string_view m)
{
    std::string out(m);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

HttpClientErr mapCprError(const cpr::Error& err)
{
    switch (err.code) {
    case cpr::ErrorCode::OPERATION_TIMEDOUT:
        return HttpClientErr::Timeout;
    case cpr::ErrorCode::EMPTY_RESPONSE:
        return HttpClientErr::EmptyResponse;
    default:
        break;
    }
    if (isTimeoutLikeMessage(err.message))
        return HttpClientErr::Timeout;
    return HttpClientErr::Transport;
}

/// Fills `HttpCallResult` from a successful CPR response (status line, headers, body, size limits).
void applyCprResponse(const cpr::Response& r, std::size_t maxBodyBytes,
    std::shared_ptr<HttpCallResult> out)
{
    out->response_ = std::make_shared<HttpResponseData>();
    out->response_->statusCode_ = static_cast<int>(r.status_code);
    out->response_->statusText_.clear();
    for (const auto& kv : r.header) {
        std::string key;
        key.reserve(kv.first.size());
        for (unsigned char c : kv.first) {
            key.push_back(static_cast<char>(std::tolower(c)));
        }
        out->response_->headers_.insert({ std::move(key), kv.second });
    }

    if (maxBodyBytes > 0) {
        if (const auto cl = parseContentLengthHeader(r.header);
            cl.has_value() && *cl > maxBodyBytes) {
            out->error_ = HttpClientErr::PayloadTooLarge;
            out->errorMessage_ = "HTTP Content-Length exceeds maxResponseBodyBytes_";
            out->response_.reset();
            return;
        }
    }

    if (maxBodyBytes > 0 && r.text.size() > maxBodyBytes) {
        out->error_ = HttpClientErr::PayloadTooLarge;
        out->errorMessage_ = "response body exceeds maxResponseBodyBytes_ after download";
        out->response_.reset();
        return;
    }

    out->response_->body_ = r.text;
    out->error_ = HttpClientErr::Ok;
    out->errorMessage_.clear();
}

cpr::Response performCprRequest(cpr::Session& session,
    const std::string& methodUpper,
    const HttpRequestSpec& spec)
{
    if (methodUpper == "GET")
        return session.Get();
    if (methodUpper == "HEAD")
        return session.Head();
    if (methodUpper == "POST") {
        if (!spec.body_.empty())
            session.SetBody(cpr::Body { spec.body_ });
        return session.Post();
    }
    if (methodUpper == "PUT") {
        if (!spec.body_.empty())
            session.SetBody(cpr::Body { spec.body_ });
        return session.Put();
    }
    if (methodUpper == "DELETE")
        return session.Delete();
    if (methodUpper == "OPTIONS")
        return session.Options();
    if (methodUpper == "PATCH") {
        if (!spec.body_.empty())
            session.SetBody(cpr::Body { spec.body_ });
        return session.Patch();
    }
    return {};
}

} // namespace

HttpClientConfig HttpClientConfig::parseOrigin(std::string_view input)
{
    std::string_view rest = trimSv(input);
    if (rest.empty())
        throw std::invalid_argument("HttpClientConfig::parseOrigin: empty input");

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

    rest = trimSv(rest);
    if (rest.empty())
        throw std::invalid_argument("HttpClientConfig::parseOrigin: missing host");

    std::uint16_t port = static_cast<std::uint16_t>(tls ? 443 : 80);
    std::string host;

    if (!rest.empty() && rest.front() == '[') {
        const std::size_t closeBracket = rest.find(']');
        if (closeBracket == std::string_view::npos || closeBracket < 2)
            throw std::invalid_argument(
                "HttpClientConfig::parseOrigin: invalid IPv6 bracket");
        host = std::string(rest.substr(1, closeBracket - 1));
        if (closeBracket + 1 < rest.size()) {
            if (rest[closeBracket + 1] != ':')
                throw std::invalid_argument(
                    "HttpClientConfig::parseOrigin: garbage after IPv6 address");
            const std::string_view portPart = rest.substr(closeBracket + 2);
            if (portPart.empty())
                throw std::invalid_argument(
                    "HttpClientConfig::parseOrigin: missing port after ':'");
            port = static_cast<std::uint16_t>(std::stoi(std::string(portPart)));
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
                port = static_cast<std::uint16_t>(std::stoi(std::string(portPart)));
            } else {
                host = std::string(rest);
            }
        } else {
            host = std::string(rest);
        }
    }

    if (host.empty())
        throw std::invalid_argument("HttpClientConfig::parseOrigin: empty host");

    HttpClientConfig out;
    out.host_ = std::move(host);
    out.port_ = port;
    out.tls_ = tls;
    return out;
}

class HttpClient::Impl {
public:
    explicit Impl(HttpClientConfig config,
        std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider)
        : cfg_(std::move(config))
        , tokenProvider_(oauth2TokenProvider
                  ? std::move(oauth2TokenProvider)
                  : std::make_shared<OAuth2TokenProvider>())
        , originBase_(buildOriginBase(cfg_))
    {
        worker_ = std::thread([this] { workerLoop(); });
        tokenProvider_->onApply([this]() { workCv_.notify_all(); });

        const char* tlsMinStr = "default";
        switch (cfg_.tlsParams_.tlsMinVersion_) {
        case TlsMinVersion::Tls12:
            tlsMinStr = "TLS1.2";
            break;
        case TlsMinVersion::Tls13:
            tlsMinStr = "TLS1.3";
            break;
        default:
            break;
        }
        BW_LOG_INFO(
            "HttpClient",
            "{} {}:{} tls={} tlsMin={} verifyPeer={} (libcpr/libcurl) "
            "retryMax={} maxPending={} maxBodyBytes={}",
            cfg_.tls_ ? "https" : "http", cfg_.host_, cfg_.port_, cfg_.tls_,
            tlsMinStr, cfg_.tlsParams_.tlsVerifyPeer_,
            cfg_.retryMaxAttempts_.has_value()
                ? static_cast<int>(*cfg_.retryMaxAttempts_)
                : 0,
            cfg_.maxPendingRequests_, cfg_.maxResponseBodyBytes_);
    }

    ~Impl() { shutdown(); }

    void shutdown() noexcept
    {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true))
            return;
        BW_LOG_INFO("HttpClient", "shutdown: stopping worker");
        workCv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    void drainStopped(std::unique_lock<std::mutex>& lk)
    {
        while (!queue_.empty()) {
            PendingJob job = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            auto res = std::make_shared<HttpCallResult>();
            res->error_ = HttpClientErr::Stopped;
            res->errorMessage_ = "HttpClient is stopped";
            safeCallback(std::move(job.cb), std::move(res));
            lk.lock();
        }
    }

    void awaitToken(std::unique_lock<std::mutex>& lk)
    {
        if (!tokenProvider_->gate().pause)
            return;
        workCv_.wait(lk, [this] {
            return stop_.load() || !tokenProvider_->gate().pause;
        });
    }

    /// Invoked with `mutex_` not held (worker has released `lk` before calling).
    void dispatchOneJobUnlocked(PendingJob job)
    {
        std::shared_ptr<HttpCallResult> res;
        if (!job.spec) {
            res = std::make_shared<HttpCallResult>();
            res->error_ = HttpClientErr::Unknown;
            res->errorMessage_ = "request: HttpRequestSpec is null";
            BW_LOG_WARN("HttpClient", "queued job missing HttpRequestSpec");
        } else {
            res = execute(*job.spec);
            if (!res->isOk()) {
                BW_LOG_WARN("HttpClient", "{} {} failed err={} {}",
                    job.spec->method_, job.spec->pathAndQuery_,
                    static_cast<int>(res->error_), res->errorMessage_);
            }
        }
        safeCallback(std::move(job.cb), std::move(res));
    }

    void workerLoop()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        while (!stop_.load()) {
            const auto deadline = tokenProvider_->gate().wakeAt;
            if (!deadline.has_value()) {
                workCv_.wait(lk, [this] {
                    return stop_.load() || !queue_.empty();
                });
            } else {
                workCv_.wait_until(lk, *deadline, [this] {
                    return stop_.load() || !queue_.empty();
                });
            }

            if (stop_.load()) {
                drainStopped(lk);
                break;
            }

            if (tokenProvider_->gate().pause) {
                lk.unlock();
                tokenProvider_->renewNotify();
                lk.lock();
                if (stop_.load()) {
                    drainStopped(lk);
                    break;
                }
                // `onRenew` often enqueues token refresh on this same client; run queued work while still
                // paused so `apply` can clear the gate before `awaitToken` would otherwise block forever.
                while (!stop_.load() && !queue_.empty() && tokenProvider_->gate().pause) {
                    PendingJob job = std::move(queue_.front());
                    queue_.pop_front();
                    lk.unlock();
                    dispatchOneJobUnlocked(std::move(job));
                    lk.lock();
                }
                awaitToken(lk);
            }

            if (stop_.load()) {
                drainStopped(lk);
                break;
            }

            while (!queue_.empty() && !stop_.load() && !tokenProvider_->gate().pause) {
                PendingJob job = std::move(queue_.front());
                queue_.pop_front();
                lk.unlock();
                dispatchOneJobUnlocked(std::move(job));
                lk.lock();
            }
        }
    }

    std::shared_ptr<HttpCallResult> execute(const HttpRequestSpec& reqIn)
    {
        HttpRequestSpec spec = reqIn;
        auto out = std::make_shared<HttpCallResult>();

        const auto bearer = tokenProvider_->accessToken();
        if (bearer.has_value() && !requestHasAuthorizationHeader(spec)) {
            spec.headers_.emplace_back("Authorization",
                std::string("Bearer ") + *bearer);
        }

        const std::string path = normalizePath(spec.pathAndQuery_);
        const std::string fullUrl = originBase_ + path;

        const int maxAttempts = cfg_.retryMaxAttempts_.has_value()
            ? static_cast<int>(*cfg_.retryMaxAttempts_) + 1
            : 1;
        std::unordered_set<int> retryCodes;
        for (int c : cfg_.retryHttpCodes_)
            retryCodes.insert(c);

        const std::string methodUpper = toUpperMethod(spec.method_);
        if (methodUpper != "GET" && methodUpper != "HEAD" && methodUpper != "POST" && methodUpper != "PUT" && methodUpper != "DELETE" && methodUpper != "OPTIONS" && methodUpper != "PATCH") {
            out->error_ = HttpClientErr::Protocol;
            out->errorMessage_ = "unsupported HTTP method for libcpr: " + spec.method_;
            return out;
        }

        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            cpr::Session session;
            session.SetUrl(cpr::Url { fullUrl });

            if (cfg_.tls_) {
                session.SetSslOptions(buildSslOptions(cfg_.tlsParams_));
            }

            cpr::Header hdr;
            if (!cfg_.userAgent_.empty()) {
                hdr["User-Agent"] = cfg_.userAgent_;
            }
            for (const auto& kv : spec.headers_) {
                if (icmpAscii(kv.first, "host"))
                    continue;
                hdr[kv.first] = kv.second;
            }
            if (!spec.body_.empty() && spec.contentType_.has_value()) {
                hdr["Content-Type"] = *spec.contentType_;
            }
            session.SetHeader(hdr);

            BW_LOG_INFO("HttpClient",
                "HTTP request attempt={}/{} {} {}\nrequest headers:\n{}request body:\n{}",
                attempt + 1,
                maxAttempts,
                methodUpper,
                fullUrl,
                formatHeadersForLog(hdr),
                spec.body_);

            const cpr::Response r = performCprRequest(session, methodUpper, spec);

            BW_LOG_INFO("HttpClient",
                "HTTP response attempt={}/{} {} status={} cpr_error.code={} cpr_error.message={}\n"
                "response headers:\n{}response body:\n{}",
                attempt + 1,
                maxAttempts,
                fullUrl,
                static_cast<int>(r.status_code),
                static_cast<int>(r.error.code),
                r.error.message,
                formatHeadersForLog(r.header),
                r.text);

            if (r.error) {
                out->error_ = mapCprError(r.error);
                out->errorMessage_ = r.error.message.empty() ? std::string("cpr error") : r.error.message;
                out->response_.reset();
                return out;
            }

            if (r.status_code == 0 && r.text.empty()) {
                out->error_ = HttpClientErr::EmptyResponse;
                out->errorMessage_ = "empty HTTP response";
                out->response_.reset();
                return out;
            }

            applyCprResponse(r, cfg_.maxResponseBodyBytes_, out);
            if (!out->isOk())
                return out;

            if (!cfg_.retryMaxAttempts_.has_value())
                return out;
            const int code = out->response_ ? out->response_->statusCode_ : 0;
            if (retryCodes.count(code) == 0 || attempt + 1 >= maxAttempts)
                return out;
            out->response_.reset();
            out->error_ = HttpClientErr::Ok;
            out->errorMessage_.clear();
            std::this_thread::sleep_for(cfg_.retryDelay_);
        }
        return out;
    }

    HttpClientConfig cfg_;
    std::mutex mutex_;
    std::condition_variable workCv_;
    std::atomic<bool> stop_ { false };
    std::thread worker_;
    std::deque<PendingJob> queue_;
    std::shared_ptr<IOAuth2TokenProvider> tokenProvider_;
    std::string originBase_;
};

HttpClient::HttpClient(HttpClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config), nullptr))
{
}

HttpClient::HttpClient(HttpClientConfig config,
    std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider)
    : impl_(std::make_unique<Impl>(std::move(config),
          std::move(oauth2TokenProvider)))
{
}

HttpClient::~HttpClient() { HttpClient::shutdown(); }

void HttpClient::shutdown() noexcept
{
    if (impl_)
        impl_->shutdown();
    impl_.reset();
}

void HttpClient::request(std::shared_ptr<HttpRequestSpec> spec,
    HttpAsyncCallback cb)
{
    if (!spec) {
        auto res = std::make_shared<HttpCallResult>();
        res->error_ = HttpClientErr::Unknown;
        res->errorMessage_ = "request: HttpRequestSpec is null";
        safeCallback(std::move(cb), std::move(res));
        return;
    }

    if (!impl_) {
        auto res = std::make_shared<HttpCallResult>();
        res->error_ = HttpClientErr::Stopped;
        res->errorMessage_ = "HttpClient is stopped";
        safeCallback(std::move(cb), std::move(res));
        return;
    }

    std::unique_lock<std::mutex> lk(impl_->mutex_);
    if (impl_->stop_.load()) {
        lk.unlock();
        auto res = std::make_shared<HttpCallResult>();
        res->error_ = HttpClientErr::Stopped;
        res->errorMessage_ = "HttpClient is stopped";
        safeCallback(std::move(cb), std::move(res));
        return;
    }

    if (impl_->cfg_.maxPendingRequests_ > 0 && impl_->queue_.size() >= impl_->cfg_.maxPendingRequests_) {
        BW_LOG_WARN(
            "HttpClient",
            "request queue full (maxPendingRequests_={}): {} {}",
            impl_->cfg_.maxPendingRequests_, spec->method_, spec->pathAndQuery_);
        lk.unlock();
        auto res = std::make_shared<HttpCallResult>();
        res->error_ = HttpClientErr::QueueFull;
        res->errorMessage_ = "pending request queue is full (see maxPendingRequests_)";
        safeCallback(std::move(cb), std::move(res));
        return;
    }

    PendingJob job;
    job.spec = std::move(spec);
    job.cb = std::move(cb);
    impl_->queue_.push_back(std::move(job));
    impl_->workCv_.notify_one();
}

std::shared_ptr<IOAuth2TokenProvider> HttpClient::oauth2TokenProvider() const
{
    if (!impl_)
        return nullptr;
    return impl_->tokenProvider_;
}

} // namespace lc
