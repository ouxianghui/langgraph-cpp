#include "foundation/network/http_client.hpp"

#include "foundation/logging/logger.hpp"
#include "foundation/network/authorization_provider.hpp"
#include "foundation/network/http_client_common.hh"
#include "foundation/network/http_observation.hh"
#include "foundation/network/http_transport.hh"
#include "foundation/rate_limit/circuit_breaker.hpp"
#include "foundation/rate_limit/rate_limiter.hpp"
#include "foundation/redaction/redactor.hpp"
#include "foundation/retry/retry_policy.hpp"
#include "foundation/network/sse_parser.hh"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lc {
using namespace http_client_detail;

class HttpClient::Impl : public std::enable_shared_from_this<HttpClient::Impl> {
public:
    explicit Impl(
        HttpClientConfig config,
        std::shared_ptr<IAuthorizationProvider> authorizationProvider,
        std::shared_ptr<ILogger> logger)
        : cfg_(std::move(config))
        , authorizationProvider_(authorizationProvider
                  ? std::move(authorizationProvider)
                  : std::make_shared<NoAuthorizationProvider>())
        , refreshableAuthorization_(std::dynamic_pointer_cast<IRefreshableAuthorization>(authorizationProvider_))
        , logger_(std::move(logger))
        , originBase_(formatOrigin(cfg_))
        , startupStatus_(cfg_.validate())
    {
        [[maybe_unused]] const char* tlsMinStr = "default";
        switch (cfg_.tlsOptions_.minVersion_) {
        case TlsMinVersion::Tls12:
            tlsMinStr = "TLS1.2";
            break;
        case TlsMinVersion::Tls13:
            tlsMinStr = "TLS1.3";
            break;
        default:
            break;
        }
        logTo(logger_,
            LogLevel::Info,
            "HttpClient",
            "{} {}:{} tls={} tlsMin={} verifyPeer={} (cpp-httplib) "
            "retryMax={} maxPending={} maxBodyBytes={} keepAlive={} poolSize={} followRedirects={} proxy={}",
            __FILE__,
            __LINE__,
            cfg_.useTls_ ? "https" : "http", cfg_.host_, cfg_.port_, cfg_.useTls_,
            tlsMinStr, cfg_.tlsOptions_.verifyPeer_,
            cfg_.retryPolicy_.maxRetries_,
            cfg_.maxPendingRequests_, cfg_.maxResponseBodyBytes_,
            cfg_.keepAlive_, cfg_.connectionPoolSize_, cfg_.followRedirects_,
            cfg_.proxy_.isEnabled());
    }

    ~Impl()
    {
        const auto status = close();
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id())
                worker_.detach();
            else if (status.isOk())
                worker_.join();
            else
                worker_.detach();
        }
    }

    Status start()
    {
        if (!startupStatus_.isOk()) {
            stop_.store(true, std::memory_order_release);
            return startupStatus_;
        }

        auto self = shared_from_this();
        try {
            if (refreshableAuthorization_) {
                refreshableAuthorization_->onReady([weak = std::weak_ptr<Impl>(self)] {
                    if (auto locked = weak.lock())
                        locked->workCv_.notify_all();
                });
            }
            worker_ = std::thread([self = std::move(self)] { self->workerLoop(); });
            return Status::ok();
        } catch (const std::exception& ex) {
            startupStatus_ = Status::unavailable(std::string("failed to start HTTP worker: ") + ex.what());
        } catch (...) {
            startupStatus_ = Status::unknown("failed to start HTTP worker");
        }
        stop_.store(true, std::memory_order_release);
        return startupStatus_;
    }

    Status close()
    {
        bool expected = false;
        if (stop_.compare_exchange_strong(expected, true))
            logTo(logger_, LogLevel::Info, "HttpClient", "close: stopping worker", __FILE__, __LINE__);

        workCv_.notify_all();
        poolCv_.notify_all();
        stopActiveClients();

        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            std::unique_lock lk(mutex_);
            const bool done = workerDoneCv_.wait_for(lk, cfg_.closeTimeout_, [this] {
                return workerDone_;
            });
            lk.unlock();
            if (!done) {
                stopActiveClients();
                return Status::deadlineExceeded("HTTP client close timed out");
            }
            worker_.join();
        } else if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) {
            worker_.detach();
        }
        return Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept
    {
        return stop_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Status startupStatus() const
    {
        return startupStatus_;
    }

    void registerActiveClient(const std::shared_ptr<httplib::Client>& client)
    {
        std::lock_guard lock(activeMutex_);
        activeClients_.erase(
            std::remove_if(activeClients_.begin(), activeClients_.end(), [](const auto& weak) {
                return weak.expired();
            }),
            activeClients_.end());
        activeClients_.push_back(client);
    }

    void unregisterActiveClient(const std::shared_ptr<httplib::Client>& client)
    {
        std::lock_guard lock(activeMutex_);
        activeClients_.erase(
            std::remove_if(activeClients_.begin(), activeClients_.end(), [&](const auto& weak) {
                auto locked = weak.lock();
                return !locked || locked == client;
            }),
            activeClients_.end());
    }

    void stopActiveClients() noexcept
    {
        std::vector<std::shared_ptr<httplib::Client>> clients;
        {
            std::lock_guard lock(activeMutex_);
            for (const auto& weak : activeClients_) {
                if (auto client = weak.lock())
                    clients.push_back(std::move(client));
            }
        }
        {
            std::lock_guard lock(poolMutex_);
            while (!idleClients_.empty()) {
                clients.push_back(std::move(idleClients_.front()));
                idleClients_.pop_front();
            }
        }
        poolCv_.notify_all();

        for (auto& client : clients) {
            try {
                client->stop();
            } catch (...) {
            }
        }
    }

    class ClientLease final {
    public:
        ClientLease() = default;

        ClientLease(
            std::shared_ptr<httplib::Client> client,
            std::function<void(std::shared_ptr<httplib::Client>, bool)> release)
            : client_(std::move(client))
            , release_(std::move(release))
        {
        }

        ~ClientLease() { release(); }

        ClientLease(const ClientLease&) = delete;
        ClientLease& operator=(const ClientLease&) = delete;

        ClientLease(ClientLease&& other) noexcept
            : client_(std::move(other.client_))
            , release_(std::move(other.release_))
            , reusable_(other.reusable_)
        {
            other.reusable_ = false;
        }

        ClientLease& operator=(ClientLease&& other) noexcept
        {
            if (this == &other)
                return *this;
            release();
            client_ = std::move(other.client_);
            release_ = std::move(other.release_);
            reusable_ = other.reusable_;
            other.reusable_ = false;
            return *this;
        }

        [[nodiscard]] httplib::Client& client() noexcept { return *client_; }
        void discard() noexcept { reusable_ = false; }

    private:
        void release() noexcept
        {
            if (!client_ || !release_)
                return;
            try {
                release_(std::move(client_), reusable_);
            } catch (...) {
            }
            release_ = {};
            reusable_ = false;
        }

        std::shared_ptr<httplib::Client> client_;
        std::function<void(std::shared_ptr<httplib::Client>, bool)> release_;
        bool reusable_ { true };
    };

    Result<ClientLease> acquireClient()
    {
        if (!cfg_.keepAlive_) {
            auto client = createClient();
            if (!client.isOk())
                return client.status();
            registerActiveClient(*client);
            if (stop_.load()) {
                try {
                    (*client)->stop();
                } catch (...) {
                }
                unregisterActiveClient(*client);
                return Status::cancelled("HTTP client is closing");
            }
            return ClientLease(std::move(*client), [this](std::shared_ptr<httplib::Client> c, bool reusable) {
                if (!reusable) {
                    try {
                        c->stop();
                    } catch (...) {
                    }
                }
                unregisterActiveClient(c);
            });
        }

        std::shared_ptr<httplib::Client> client;
        {
            std::unique_lock lock(poolMutex_);
            poolCv_.wait(lock, [this] {
                return stop_.load()
                    || !idleClients_.empty()
                    || leasedClients_ < cfg_.connectionPoolSize_;
            });

            if (stop_.load())
                return Status::cancelled("HTTP client is closing");

            if (!idleClients_.empty()) {
                client = std::move(idleClients_.front());
                idleClients_.pop_front();
            } else {
                auto created = createClient();
                if (!created.isOk())
                    return created.status();
                client = std::move(*created);
            }
            ++leasedClients_;
        }

        registerActiveClient(client);
        if (stop_.load()) {
            releasePooledClient(client, false);
            return Status::cancelled("HTTP client is closing");
        }
        return ClientLease(std::move(client), [this](std::shared_ptr<httplib::Client> c, bool reusable) {
            releasePooledClient(std::move(c), reusable);
        });
    }

    Result<std::shared_ptr<httplib::Client>> createClient()
    {
        try {
            auto client = std::make_shared<httplib::Client>(originBase_);
            configureClient(*client, cfg_);
            return client;
        } catch (const std::bad_alloc&) {
            return Status::resourceExhausted("failed to allocate HTTP client");
        } catch (const std::exception& ex) {
            return Status::internal(std::string("failed to configure HTTP client: ") + ex.what());
        } catch (...) {
            return Status::unknown("failed to configure HTTP client");
        }
    }

    void releasePooledClient(std::shared_ptr<httplib::Client> client, bool reusable) noexcept
    {
        unregisterActiveClient(client);
        if (!reusable) {
            try {
                client->stop();
            } catch (...) {
            }
        }

        {
            std::lock_guard lock(poolMutex_);
            if (leasedClients_ > 0)
                --leasedClients_;
            if (reusable && !stop_.load() && idleClients_.size() < cfg_.connectionPoolSize_) {
                idleClients_.push_back(std::move(client));
            }
        }
        poolCv_.notify_one();
    }

    [[nodiscard]] bool waitBeforeRetry(Clock::Duration delay)
    {
        if (delay <= Clock::Duration::zero())
            return !stop_.load();
        std::unique_lock lock(mutex_);
        return !workCv_.wait_for(lock, delay, [this] { return stop_.load(); });
    }

    void drainStopped(std::unique_lock<std::mutex>& lk)
    {
        while (!queue_.empty()) {
            PendingJob job = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            invokeCallback(std::move(job.cb), Status::failedPrecondition("HttpClient is stopped"));
            lk.lock();
        }
    }

    void awaitAuthorization(std::unique_lock<std::mutex>& lk)
    {
        if (!authorizationGate().paused_)
            return;
        workCv_.wait(lk, [this] {
            return stop_.load() || !authorizationGate().paused_;
        });
    }

    [[nodiscard]] AuthorizationGate authorizationGate() const
    {
        if (!refreshableAuthorization_)
            return {};
        return refreshableAuthorization_->gate();
    }

    void requestAuthorizationRefresh()
    {
        if (refreshableAuthorization_)
            refreshableAuthorization_->requestRefresh();
    }

    void dispatchOneJobUnlocked(PendingJob job)
    {
        auto result = execute(job.request);
        if (!result.isOk()) {
            logTo(logger_,
                LogLevel::Warn,
                "HttpClient",
                "{} {} failed: {}",
                __FILE__,
                __LINE__,
                httpMethodName(job.request.method_),
                job.request.pathAndQuery_,
                result.status().toString());
        }
        invokeCallback(std::move(job.cb), std::move(result));
    }

    void workerLoop()
    {
        {
            std::unique_lock<std::mutex> lk(mutex_);
            while (!stop_.load()) {
                const auto deadline = authorizationGate().resumeAt_;
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

                if (authorizationGate().paused_) {
                    lk.unlock();
                    requestAuthorizationRefresh();
                    lk.lock();
                    if (stop_.load()) {
                        drainStopped(lk);
                        break;
                    }
                    awaitAuthorization(lk);
                }

                if (stop_.load()) {
                    drainStopped(lk);
                    break;
                }

                while (!queue_.empty()
                    && !stop_.load()
                    && !authorizationGate().paused_) {
                    PendingJob job = std::move(queue_.front());
                    queue_.pop_front();
                    lk.unlock();
                    dispatchOneJobUnlocked(std::move(job));
                    lk.lock();
                }
            }
        }

        {
            std::lock_guard lock(mutex_);
            workerDone_ = true;
        }
        workerDoneCv_.notify_all();
    }

    HttpResult execute(const HttpRequest& requestIn)
    {
        if (!startupStatus_.isOk())
            return startupStatus_;
        if (stop_.load())
            return Status::cancelled("HTTP client is closing");

        HttpRequest request = requestIn;
        if (auto status = validateRequest(request, cfg_.requestLimits_); !status.isOk())
            return status;

        if (auto status = authorizationProvider_->authorize(request); !status.isOk())
            return status;
        if (auto status = validateRequest(request, cfg_.requestLimits_); !status.isOk())
            return status;

        if (!httpMethodAllowsBody(request.method_) && !request.body_.empty())
            return Status::invalidArgument("HTTP request method does not allow a body");

        const std::string path = normalizePath(request.pathAndQuery_);
        const std::string fullUrl = originBase_ + path;
        HttpObservation observation(
            cfg_.metrics_,
            cfg_.traceSink_,
            request.method_,
            cfg_.host_,
            cfg_.port_,
            redactor_.redact(fullUrl));

        auto fail = [&](Status status) -> HttpResult {
            observation.error(status);
            return status;
        };
        auto finishResponse = [&](HttpResult response) -> HttpResult {
            if (response.isOk())
                observation.response(response->statusCode_, response->body_.size());
            else
                observation.error(response.status());
            return response;
        };

        const auto retryPolicy = retryPolicyFromHttp(cfg_.retryPolicy_);
        const auto maxAttempts = retryPolicy.maxAttempts();

#ifndef CPPHTTPLIB_SSL_ENABLED
        if (cfg_.useTls_) {
            return fail(Status::failedPrecondition("HTTPS requested but HTTP client was built without OpenSSL support"));
        }
#endif

        for (std::uint32_t attempt = 1; attempt <= maxAttempts; ++attempt) {
            if (stop_.load())
                return fail(Status::cancelled("HTTP client is closing"));

            if (cfg_.rateLimiter_) {
                const auto rate = cfg_.rateLimiter_->acquire();
                if (!rate.allowed_)
                    return fail(rate.status_);
            }

            if (cfg_.circuitBreaker_) {
                const auto circuit = cfg_.circuitBreaker_->acquire();
                if (!circuit.allowed_)
                    return fail(circuit.status_);
            }

            auto hdr = buildRequestHeaders(cfg_, request);

            logTo(logger_,
                LogLevel::Info,
                "HttpClient",
                "HTTP request attempt={}/{} {} {}\nrequest headers:\n{}request body:\n{}",
                __FILE__,
                __LINE__,
                attempt,
                maxAttempts,
                httpMethodName(request.method_),
                redactor_.redact(fullUrl),
                formatHeadersForLog(hdr, cfg_.logOptions_, redactor_),
                formatBodyForLog(request.body_, cfg_.logOptions_, redactor_));

            auto leaseResult = acquireClient();
            if (!leaseResult.isOk())
                return fail(leaseResult.status());

            httplib::Result r;
            {
                auto lease = std::move(*leaseResult);
                r = performRequest(lease.client(), request.method_, path, hdr, request);
                if (!r)
                    lease.discard();
            }

            logTo(logger_,
                LogLevel::Info,
                "HttpClient",
                "HTTP response attempt={}/{} {} result={} error={}\n"
                "response headers:\n{}response body:\n{}",
                __FILE__,
                __LINE__,
                attempt,
                maxAttempts,
                redactor_.redact(fullUrl),
                r ? static_cast<int>(r->status) : 0,
                httplib::to_string(r.error()),
                r ? formatHeadersForLog(r->headers, cfg_.logOptions_, redactor_) : std::string(),
                r ? formatBodyForLog(r->body, cfg_.logOptions_, redactor_) : std::string());

            if (!r) {
                const auto status = stop_.load()
                    ? Status::cancelled("HTTP request cancelled")
                    : statusFromHttplibResultFailure(r.error(), cfg_.maxResponseBodyBytes_);
                if (cfg_.circuitBreaker_)
                    cfg_.circuitBreaker_->recordFailure();
                const auto decision = retryPolicy.decide(status, attempt);
                if (!decision.isOk())
                    return fail(decision.status());
                if (!decision->retry_)
                    return fail(status);
                if (!waitBeforeRetry(decision->delay_))
                    return fail(Status::cancelled("HTTP client is closing"));
                continue;
            }

            if (r->status <= 0 && r->body.empty()) {
                const auto status = Status::unavailable("empty HTTP response");
                if (cfg_.circuitBreaker_)
                    cfg_.circuitBreaker_->recordFailure();
                const auto decision = retryPolicy.decide(status, attempt);
                if (!decision.isOk())
                    return fail(decision.status());
                if (!decision->retry_)
                    return fail(status);
                if (!waitBeforeRetry(decision->delay_))
                    return fail(Status::cancelled("HTTP client is closing"));
                continue;
            }

            auto response = buildHttpResponse(*r, cfg_.maxResponseBodyBytes_);
            if (!response.isOk()) {
                if (cfg_.circuitBreaker_)
                    cfg_.circuitBreaker_->recordFailure();
                return fail(response.status());
            }

            const int code = response->statusCode_;
            if (cfg_.circuitBreaker_) {
                if (circuitFailureStatus(code))
                    cfg_.circuitBreaker_->recordFailure();
                else
                    cfg_.circuitBreaker_->recordSuccess();
            }

            if (!containsHttpRetryCode(cfg_.retryPolicy_, code))
                return finishResponse(std::move(response));

            const auto status = statusForHttpRetry(code);
            const auto decision = retryPolicy.decide(status, attempt);
            if (!decision.isOk())
                return fail(decision.status());
            if (!decision->retry_)
                return finishResponse(std::move(response));
            if (!waitBeforeRetry(decision->delay_))
                return fail(Status::cancelled("HTTP client is closing"));
        }
        return fail(Status::unknown("HTTP request did not complete"));
    }

    HttpResult executeStreaming(
        const HttpRequest& requestIn,
        HttpBodyChunkCallback callback,
        HttpStreamOptions options)
    {
        if (!callback)
            return Status::invalidArgument("HTTP stream callback is required");
        if (auto status = options.validate(); !status.isOk())
            return status;
        if (!startupStatus_.isOk())
            return startupStatus_;
        if (stop_.load())
            return Status::cancelled("HTTP client is closing");

        HttpRequest request = requestIn;
        if (auto status = validateRequest(request, cfg_.requestLimits_); !status.isOk())
            return status;

        if (auto status = authorizationProvider_->authorize(request); !status.isOk())
            return status;
        if (auto status = validateRequest(request, cfg_.requestLimits_); !status.isOk())
            return status;

        if (!httpMethodAllowsBody(request.method_) && !request.body_.empty())
            return Status::invalidArgument("HTTP request method does not allow a body");

        const std::string path = normalizePath(request.pathAndQuery_);
        const std::string fullUrl = originBase_ + path;
        HttpObservation observation(
            cfg_.metrics_,
            cfg_.traceSink_,
            request.method_,
            cfg_.host_,
            cfg_.port_,
            redactor_.redact(fullUrl));

        auto fail = [&](Status status) -> HttpResult {
            observation.error(status);
            return status;
        };

#ifndef CPPHTTPLIB_SSL_ENABLED
        if (cfg_.useTls_) {
            return fail(Status::failedPrecondition("HTTPS requested but HTTP client was built without OpenSSL support"));
        }
#endif

        if (cfg_.rateLimiter_) {
            const auto rate = cfg_.rateLimiter_->acquire();
            if (!rate.allowed_)
                return fail(rate.status_);
        }

        if (cfg_.circuitBreaker_) {
            const auto circuit = cfg_.circuitBreaker_->acquire();
            if (!circuit.allowed_)
                return fail(circuit.status_);
        }

        auto clientResult = createClient();
        if (!clientResult.isOk())
            return fail(clientResult.status());

        auto client = std::move(*clientResult);
        registerActiveClient(client);
        struct ActiveClientGuard {
            Impl* self_ {};
            std::shared_ptr<httplib::Client> client_;
            ~ActiveClientGuard()
            {
                if (self_ && client_)
                    self_->unregisterActiveClient(client_);
            }
        } guard { this, client };

        if (stop_.load()) {
            try {
                client->stop();
            } catch (...) {
            }
            return fail(Status::cancelled("HTTP client is closing"));
        }

        const auto hdr = buildRequestHeaders(cfg_, request);
        logTo(logger_,
            LogLevel::Info,
            "HttpClient",
            "HTTP stream request {} {}\nrequest headers:\n{}request body:\n{}",
            __FILE__,
            __LINE__,
            httpMethodName(request.method_),
            redactor_.redact(fullUrl),
            formatHeadersForLog(hdr, cfg_.logOptions_, redactor_),
            formatBodyForLog(request.body_, cfg_.logOptions_, redactor_));

        auto stream = performStreamingRequest(*client, request.method_, path, hdr, request, options);
        if (!stream) {
            const auto status = stop_.load()
                ? Status::cancelled("HTTP request cancelled")
                : statusFromHttplibError(stream.error());
            if (cfg_.circuitBreaker_)
                cfg_.circuitBreaker_->recordFailure();
            return fail(status);
        }

        auto response = buildStreamingHttpResponse(stream, cfg_.maxResponseBodyBytes_);
        if (!response.isOk()) {
            if (cfg_.circuitBreaker_)
                cfg_.circuitBreaker_->recordFailure();
            return fail(response.status());
        }

        std::size_t responseBytes = 0;
        Status streamStatus = Status::ok();
        while (stream.next()) {
            if (stop_.load()) {
                streamStatus = Status::cancelled("HTTP client is closing");
                break;
            }

            const std::size_t chunkSize = stream.size();
            if (cfg_.maxResponseBodyBytes_ > 0
                && (chunkSize > cfg_.maxResponseBodyBytes_
                    || responseBytes > cfg_.maxResponseBodyBytes_ - chunkSize)) {
                streamStatus = Status::resourceExhausted("HTTP response body exceeds maxResponseBodyBytes");
                break;
            }
            responseBytes += chunkSize;

            try {
                streamStatus = callback(std::string_view(stream.data(), chunkSize));
            } catch (const std::exception& ex) {
                streamStatus = Status::unknown(std::string("HTTP stream callback failed: ") + ex.what());
            } catch (...) {
                streamStatus = Status::unknown("HTTP stream callback failed");
            }

            if (!streamStatus.isOk())
                break;
        }

        if (!streamStatus.isOk()) {
            try {
                client->stop();
            } catch (...) {
            }
            if (cfg_.circuitBreaker_)
                cfg_.circuitBreaker_->recordFailure();
            return fail(std::move(streamStatus));
        }

        if (stream.has_read_error()) {
            const auto status = statusFromHttplibError(stream.read_error());
            if (cfg_.circuitBreaker_)
                cfg_.circuitBreaker_->recordFailure();
            return fail(status);
        }

        const int code = response->statusCode_;
        if (cfg_.circuitBreaker_) {
            if (circuitFailureStatus(code))
                cfg_.circuitBreaker_->recordFailure();
            else
                cfg_.circuitBreaker_->recordSuccess();
        }

        observation.response(code, responseBytes);
        logTo(logger_,
            LogLevel::Info,
            "HttpClient",
            "HTTP stream response {} status={} bytes={}\nresponse headers:\n{}",
            __FILE__,
            __LINE__,
            redactor_.redact(fullUrl),
            code,
            responseBytes,
            formatHeadersForLog(stream.headers(), cfg_.logOptions_, redactor_));
        return response;
    }

    HttpClientConfig cfg_;
    std::mutex mutex_;
    std::condition_variable workCv_;
    std::condition_variable workerDoneCv_;
    std::atomic<bool> stop_ { false };
    std::thread worker_;
    std::deque<PendingJob> queue_;
    bool workerDone_ { false };
    std::mutex activeMutex_;
    std::vector<std::weak_ptr<httplib::Client>> activeClients_;
    std::mutex poolMutex_;
    std::condition_variable poolCv_;
    std::deque<std::shared_ptr<httplib::Client>> idleClients_;
    std::size_t leasedClients_ { 0 };
    std::shared_ptr<IAuthorizationProvider> authorizationProvider_;
    std::shared_ptr<IRefreshableAuthorization> refreshableAuthorization_;
    std::shared_ptr<ILogger> logger_;
    std::string originBase_;
    Redactor redactor_;
    Status startupStatus_ { Status::ok() };
};

HttpClient::HttpClient(HttpClientConfig config, std::shared_ptr<ILogger> logger)
{
    auto impl = std::make_shared<Impl>(std::move(config), nullptr, std::move(logger));
    impl->start();
    std::lock_guard lock(mutex_);
    impl_ = std::move(impl);
}

HttpClient::HttpClient(HttpClientConfig config,
    std::shared_ptr<IAuthorizationProvider> authorizationProvider,
    std::shared_ptr<ILogger> logger)
{
    auto impl = std::make_shared<Impl>(
        std::move(config),
        std::move(authorizationProvider),
        std::move(logger));
    impl->start();
    std::lock_guard lock(mutex_);
    impl_ = std::move(impl);
}

HttpClient::~HttpClient() { (void)HttpClient::close(); }

Status HttpClient::close()
{
    std::shared_ptr<Impl> impl;
    {
        std::lock_guard lock(mutex_);
        impl = std::move(impl_);
        impl_.reset();
    }
    if (impl)
        return impl->close();
    return Status::ok();
}

bool HttpClient::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return !impl_ || impl_->isClosed();
}

HttpResult HttpClient::send(HttpRequest request)
{
    std::shared_ptr<Impl> impl;
    {
        std::lock_guard lock(mutex_);
        impl = impl_;
    }
    if (!impl)
        return Status::failedPrecondition("HttpClient is stopped");
    if (!impl->startupStatus().isOk())
        return impl->startupStatus();
    if (impl->stop_.load())
        return Status::failedPrecondition("HttpClient is stopped");

    if (impl->authorizationGate().paused_) {
        impl->requestAuthorizationRefresh();
        return Status::unauthenticated("authorization renewal required before sending HTTP request");
    }

    return impl->execute(request);
}

HttpResult HttpClient::sendStreaming(
    HttpRequest request,
    HttpBodyChunkCallback callback,
    HttpStreamOptions options)
{
    std::shared_ptr<Impl> impl;
    {
        std::lock_guard lock(mutex_);
        impl = impl_;
    }
    if (!impl)
        return Status::failedPrecondition("HttpClient is stopped");
    if (!impl->startupStatus().isOk())
        return impl->startupStatus();
    if (impl->stop_.load())
        return Status::failedPrecondition("HttpClient is stopped");

    if (impl->authorizationGate().paused_) {
        impl->requestAuthorizationRefresh();
        return Status::unauthenticated("authorization renewal required before streaming HTTP request");
    }

    return impl->executeStreaming(request, std::move(callback), options);
}

HttpResult HttpClient::sendSse(
    HttpRequest request,
    ServerSentEventCallback callback,
    HttpStreamOptions options)
{
    if (!callback)
        return Status::invalidArgument("SSE callback is required");
    if (!request.header("accept").has_value())
        request.setHeader("Accept", "text/event-stream");

    SseParser parser;
    auto response = sendStreaming(
        std::move(request),
        [&](std::string_view chunk) {
            return parser.feed(chunk, callback);
        },
        options);
    if (!response.isOk())
        return response;

    if (auto status = parser.finish(callback); !status.isOk())
        return status;
    return response;
}

Status HttpClient::sendAsync(HttpRequest request, HttpCallback cb)
{
    std::shared_ptr<Impl> impl;
    {
        std::lock_guard lock(mutex_);
        impl = impl_;
    }
    if (!impl)
        return Status::failedPrecondition("HttpClient is stopped");
    if (!impl->startupStatus().isOk())
        return impl->startupStatus();

    std::unique_lock<std::mutex> lk(impl->mutex_);
    if (impl->stop_.load())
        return Status::failedPrecondition("HttpClient is stopped");

    if (impl->cfg_.maxPendingRequests_ > 0 && impl->queue_.size() >= impl->cfg_.maxPendingRequests_) {
        logTo(impl->logger_,
            LogLevel::Warn,
            "HttpClient",
            "request queue full (maxPendingRequests_={}): {} {}",
            __FILE__,
            __LINE__,
            impl->cfg_.maxPendingRequests_,
            httpMethodName(request.method_),
            request.pathAndQuery_);
        return Status::resourceExhausted("pending HTTP request queue is full");
    }

    PendingJob job;
    job.request = std::move(request);
    job.cb = std::move(cb);
    impl->queue_.push_back(std::move(job));
    impl->workCv_.notify_one();
    return Status::ok();
}

std::shared_ptr<IAuthorizationProvider> HttpClient::authorizationProvider() const
{
    std::lock_guard lock(mutex_);
    if (!impl_)
        return nullptr;
    return impl_->authorizationProvider_;
}

} // namespace lc
