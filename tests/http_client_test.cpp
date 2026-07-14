#include "foundation/network/authorization_provider.hpp"
#include "foundation/network/http_client.hpp"
#include "foundation/network/http_client_factory.hpp"
#include "foundation/network/http_client_types.hpp"
#include "foundation/observability/metrics.hpp"
#include "foundation/observability/tracing.hpp"
#include "foundation/rate_limit/circuit_breaker.hpp"
#include "foundation/rate_limit/rate_limiter.hpp"
#include "foundation/time/clock.hpp"

#include <httplib.h>

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef CPPHTTPLIB_SSL_ENABLED
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

namespace {

class MemoryLogger final : public lc::ILogger {
public:
    void log(const lc::LogRecord& record) noexcept override
    {
        std::lock_guard lock(mutex_);
        records_.push_back(record);
    }

    lc::Status flush() override { return lc::Status::ok(); }
    lc::Status close() override { return lc::Status::ok(); }
    bool isClosed() const noexcept override { return false; }

    std::string text() const
    {
        std::lock_guard lock(mutex_);
        std::string out;
        for (const auto& record : records_) {
            out.append(record.message_);
            out.push_back('\n');
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::vector<lc::LogRecord> records_;
};

class LocalHttpServer final {
public:
    using Factory = std::function<std::unique_ptr<httplib::Server>()>;

    explicit LocalHttpServer(Factory factory)
    {
        for (int port = 18080; port < 18140; ++port) {
            {
                std::lock_guard lock(portMutex());
                if (reservedPorts().contains(port))
                    continue;
            }

            auto candidate = factory();
            assert(candidate);
            candidate->set_address_family(AF_INET);
            if (!candidate->bind_to_port("127.0.0.1", port))
                continue;

            {
                std::lock_guard lock(portMutex());
                reservedPorts().insert(port);
            }
            server_ = std::move(candidate);
            port_ = port;
            break;
        }

        if (!server_)
            return;
        assert(port_ > 0);
        thread_ = std::thread([this] {
            (void)server_->listen_after_bind();
        });
        for (int i = 0; i < 100 && !server_->is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(server_->is_running());
    }

    ~LocalHttpServer()
    {
        if (!server_)
            return;
        server_->stop();
        if (thread_.joinable())
            thread_.join();
        {
            std::lock_guard lock(portMutex());
            reservedPorts().erase(port_);
        }
    }

    bool available() const noexcept { return server_ != nullptr && port_ > 0; }
    int port() const noexcept { return port_; }

private:
    static std::mutex& portMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    static std::set<int>& reservedPorts()
    {
        static std::set<int> ports;
        return ports;
    }

    std::unique_ptr<httplib::Server> server_;
    int port_ { 0 };
    std::thread thread_;
};

#ifdef CPPHTTPLIB_SSL_ENABLED
class TestTlsMaterial final {
public:
    TestTlsMaterial()
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!ctx)
            return;
        if (EVP_PKEY_keygen_init(ctx) <= 0
            || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0
            || EVP_PKEY_keygen(ctx, &key_) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            clear();
            return;
        }
        EVP_PKEY_CTX_free(ctx);

        cert_ = X509_new();
        if (!cert_) {
            clear();
            return;
        }

        ASN1_INTEGER_set(X509_get_serialNumber(cert_), 1);
        X509_gmtime_adj(X509_get_notBefore(cert_), -60);
        X509_gmtime_adj(X509_get_notAfter(cert_), 60 * 60);
        X509_set_version(cert_, 2);
        if (X509_set_pubkey(cert_, key_) != 1) {
            clear();
            return;
        }

        X509_NAME* name = X509_get_subject_name(cert_);
        X509_NAME_add_entry_by_txt(
            name,
            "CN",
            MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"),
            -1,
            -1,
            0);
        X509_set_issuer_name(cert_, name);
        if (X509_sign(cert_, key_, EVP_sha256()) <= 0) {
            clear();
            return;
        }

        BIO* certBio = BIO_new(BIO_s_mem());
        BIO* keyBio = BIO_new(BIO_s_mem());
        if (!certBio || !keyBio
            || PEM_write_bio_X509(certBio, cert_) != 1
            || PEM_write_bio_PrivateKey(keyBio, key_, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            if (certBio)
                BIO_free(certBio);
            if (keyBio)
                BIO_free(keyBio);
            clear();
            return;
        }

        char* certData = nullptr;
        char* keyData = nullptr;
        const long certLen = BIO_get_mem_data(certBio, &certData);
        const long keyLen = BIO_get_mem_data(keyBio, &keyData);
        if (certLen > 0 && keyLen > 0 && certData && keyData) {
            certPem_.assign(certData, static_cast<std::size_t>(certLen));
            keyPem_.assign(keyData, static_cast<std::size_t>(keyLen));
        }
        BIO_free(certBio);
        BIO_free(keyBio);
    }

    ~TestTlsMaterial() { clear(); }

    TestTlsMaterial(const TestTlsMaterial&) = delete;
    TestTlsMaterial& operator=(const TestTlsMaterial&) = delete;

    bool valid() const noexcept { return !certPem_.empty() && !keyPem_.empty(); }

    httplib::SSLServer::PemMemory pem() const noexcept
    {
        return httplib::SSLServer::PemMemory {
            .cert_pem = certPem_.data(),
            .cert_pem_len = certPem_.size(),
            .key_pem = keyPem_.data(),
            .key_pem_len = keyPem_.size(),
            .client_ca_pem = nullptr,
            .client_ca_pem_len = 0,
            .private_key_password = nullptr,
        };
    }

private:
    void clear() noexcept
    {
        if (cert_) {
            X509_free(cert_);
            cert_ = nullptr;
        }
        if (key_) {
            EVP_PKEY_free(key_);
            key_ = nullptr;
        }
    }

    X509* cert_ { nullptr };
    EVP_PKEY* key_ { nullptr };
    std::string certPem_;
    std::string keyPem_;
};
#endif

} // namespace

int main()
{
    auto https = lc::HttpClientConfig::fromOrigin("https://api.example.com/v1/chat");
    assert(https.isOk());
    assert(https->host_ == "api.example.com");
    assert(https->port_ == 443);
    assert(https->useTls_);

    auto http = lc::HttpClientConfig::fromOrigin("http://localhost:8080");
    assert(http.isOk());
    assert(http->host_ == "localhost");
    assert(http->port_ == 8080);
    assert(!http->useTls_);

    auto ipv6 = lc::HttpClientConfig::fromOrigin("https://[::1]:8443/path");
    assert(ipv6.isOk());
    assert(ipv6->host_ == "::1");
    assert(ipv6->port_ == 8443);
    assert(ipv6->useTls_);

    auto invalid = lc::HttpClientConfig::fromOrigin("https://host:999999");
    assert(!invalid.isOk());
    assert(invalid.status().code() == lc::StatusCode::InvalidArgument);

    auto request = lc::HttpRequest::post("/v1/messages", R"({"hello":"world"})");
    assert(request.method_ == lc::HttpMethod::Post);
    assert(request.contentType_ == "application/json");
    request.setHeader("Authorization", "Bearer one");
    request.setHeader("authorization", "Bearer two");
    auto header = request.header("AUTHORIZATION");
    assert(header.has_value());
    assert(*header == "Bearer two");
    assert(lc::httpMethodName(lc::HttpMethod::Patch) == "PATCH");
    assert(lc::httpMethodAllowsBody(lc::HttpMethod::Post));
    assert(!lc::httpMethodAllowsBody(lc::HttpMethod::Get));

    lc::HttpResponse response {
        .statusCode_ = 200,
        .reason_ = "OK",
        .headers_ = { { "Content-Type", "application/json" } },
        .body_ = "{}",
    };
    assert(response.isSuccessful());
    assert(!response.isClientError());
    assert(response.header("content-type") == "application/json");

    lc::HttpClientConfig config;
    config.host_ = "127.0.0.1";
    config.port_ = 9;
    config.connectTimeout_ = std::chrono::milliseconds(1);
    config.readTimeout_ = std::chrono::milliseconds(1);
    config.writeTimeout_ = std::chrono::milliseconds(1);
    assert(config.validate().isOk());

    auto invalidConfig = config;
    invalidConfig.host_.clear();
    assert(invalidConfig.validate().code() == lc::StatusCode::InvalidArgument);

    invalidConfig = config;
    invalidConfig.closeTimeout_ = std::chrono::milliseconds(0);
    assert(invalidConfig.validate().code() == lc::StatusCode::InvalidArgument);

    invalidConfig = config;
    invalidConfig.userAgent_ = "bad\r\nagent";
    assert(invalidConfig.validate().code() == lc::StatusCode::InvalidArgument);

    invalidConfig = config;
    invalidConfig.proxy_.host_ = "proxy.local";
    assert(invalidConfig.validate().code() == lc::StatusCode::InvalidArgument);

    invalidConfig = config;
    invalidConfig.proxy_.host_ = "proxy.local";
    invalidConfig.proxy_.port_ = 8080;
    invalidConfig.proxy_.username_ = "user";
    assert(invalidConfig.validate().code() == lc::StatusCode::InvalidArgument);

    invalidConfig = config;
    invalidConfig.keepAlive_ = true;
    invalidConfig.connectionPoolSize_ = 0;
    assert(invalidConfig.validate().code() == lc::StatusCode::InvalidArgument);

    invalidConfig = config;
    invalidConfig.connectionPoolSize_ = 257;
    assert(invalidConfig.validate().code() == lc::StatusCode::OutOfRange);

    lc::HttpRequestOptions invalidOptions;
    invalidOptions.timeout_ = -std::chrono::milliseconds(1);
    assert(invalidOptions.validate().code() == lc::StatusCode::InvalidArgument);
    invalidOptions.timeout_.reset();
    invalidOptions.retryPolicy_ = lc::HttpRetryPolicy {
        .statusCodes_ = { 99 },
    };
    assert(invalidOptions.validate().code() == lc::StatusCode::InvalidArgument);

    auto proxyConfig = config;
    proxyConfig.proxy_.host_ = "proxy.local";
    proxyConfig.proxy_.port_ = 8080;
    proxyConfig.proxy_.auth_ = lc::HttpProxyAuth::Basic;
    proxyConfig.proxy_.username_ = "user";
    proxyConfig.proxy_.password_ = "pass";
    assert(proxyConfig.validate().isOk());

    lc::HttpClient client(config);
    assert(!client.isClosed());
    assert(client.close().isOk());
    assert(client.isClosed());
    auto stopped = client.send(lc::HttpRequest::get("/"), lc::HttpRequestOptions {});
    assert(!stopped.isOk());
    assert(stopped.status().code() == lc::StatusCode::FailedPrecondition);

    bool callbackCalled = false;
    auto asyncStopped = client.sendAsync(lc::HttpRequest::get("/"), lc::HttpRequestOptions {}, [&](lc::HttpResult) {
        callbackCalled = true;
    });
    assert(!asyncStopped.isOk());
    assert(asyncStopped.code() == lc::StatusCode::FailedPrecondition);
    assert(!callbackCalled);

    {
        lc::HttpClient open(config);
        auto invalidBody = lc::HttpRequest::get("/");
        invalidBody.body_ = "not allowed";
        auto result = open.send(std::move(invalidBody), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::InvalidArgument);
    }

    {
        lc::HttpClient open(config);
        auto invalidHeader = lc::HttpRequest::get("/");
        invalidHeader.setHeader("bad header", "x");
        auto result = open.send(std::move(invalidHeader), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::InvalidArgument);
    }

    {
        auto limitedConfig = config;
        limitedConfig.requestLimits_.maxRequestBodyBytes_ = 4;
        lc::HttpClient open(limitedConfig);
        auto result = open.send(lc::HttpRequest::post("/", "12345", "text/plain"), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
        auto limitedConfig = config;
        limitedConfig.requestLimits_.maxHeaderCount_ = 1;
        lc::HttpClient open(limitedConfig);
        auto tooManyHeaders = lc::HttpRequest::get("/");
        tooManyHeaders.setHeader("X-One", "1");
        tooManyHeaders.setHeader("X-Two", "2");
        auto result = open.send(std::move(tooManyHeaders), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
        auto auth = std::make_shared<lc::OAuthAuthorizationProvider>(std::chrono::seconds(60));
        auto gatedConfig = config;
        gatedConfig.maxPendingRequests_ = 1;
        lc::HttpClient gated(gatedConfig, auth);
        assert(auth->apply(lc::OAuthCredentials {
            .accessToken_ = "expired",
            .accessTokenExpiresAt_ = std::chrono::system_clock::now() - std::chrono::seconds(1),
        }).isOk());

        std::atomic<bool> asyncDone { false };
        auto queued = gated.sendAsync(lc::HttpRequest::get("/"), lc::HttpRequestOptions {}, [&](lc::HttpResult) {
            asyncDone = true;
        });
        assert(queued.isOk());
        auto rejected = gated.sendAsync(lc::HttpRequest::get("/"), lc::HttpRequestOptions {}, [&](lc::HttpResult) {});
        assert(!rejected.isOk());
        assert(rejected.code() == lc::StatusCode::ResourceExhausted);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!asyncDone.load());

        assert(auth->apply(lc::OAuthCredentials {
            .accessToken_ = "fresh",
            .accessTokenExpiresAt_ = std::chrono::system_clock::now() + std::chrono::hours(1),
        }).isOk());
        for (int i = 0; i < 100 && !asyncDone.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(asyncDone.load());
    }

    {
        auto auth = std::make_shared<lc::OAuthAuthorizationProvider>(std::chrono::seconds(60));
        auth->onRenew([](lc::OAuthTokenRefreshReason) {
            throw std::runtime_error("renew callback failed");
        });
        assert(auth->apply(lc::OAuthCredentials {
            .accessToken_ = "expired",
            .accessTokenExpiresAt_ = std::chrono::system_clock::now() - std::chrono::seconds(1),
        }).isOk());
        lc::HttpClient gated(config, auth);
        auto result = gated.send(lc::HttpRequest::get("/"), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::Unauthenticated);
    }

    {
        lc::ManualWallClock wall(lc::WallClock::TimePoint(std::chrono::seconds(100)));
        lc::OAuthAuthorizationProvider auth(wall, std::chrono::seconds(10));

        auto invalidLead = auth.apply(lc::OAuthCredentials {
            .accessToken_ = "token",
            .accessTokenExpiresAt_ = wall.now() + std::chrono::seconds(60),
            .tokenRenewLeadTime_ = std::chrono::seconds(-1),
        });
        assert(!invalidLead.isOk());
        assert(invalidLead.code() == lc::StatusCode::InvalidArgument);

        auto missingToken = auth.apply(lc::OAuthCredentials {
            .accessTokenExpiresAt_ = wall.now() + std::chrono::seconds(60),
        });
        assert(!missingToken.isOk());
        assert(missingToken.code() == lc::StatusCode::InvalidArgument);

        auth.onReady([] {
            throw std::runtime_error("apply callback failed");
        });
        auto callbackFailure = auth.apply(lc::OAuthCredentials {
            .accessToken_ = "token",
            .accessTokenExpiresAt_ = wall.now() + std::chrono::seconds(60),
        });
        assert(!callbackFailure.isOk());
        assert(callbackFailure.code() == lc::StatusCode::Unknown);

        auth.onReady({});
        assert(auth.apply(lc::OAuthCredentials {
            .accessToken_ = "token",
            .refreshToken_ = "refresh",
            .accessTokenExpiresAt_ = wall.now() + std::chrono::seconds(60),
        }).isOk());
        assert(auth.refreshToken() == "refresh");
        assert(!auth.gate().paused_);
        wall.advance(std::chrono::seconds(51));
        assert(auth.gate().paused_);
    }

    {
        lc::HttpClient callbackClose(config);
        std::atomic<bool> called { false };
        auto queued = callbackClose.sendAsync(lc::HttpRequest::get("/"), lc::HttpRequestOptions {}, [&](lc::HttpResult) {
            assert(callbackClose.close().isOk());
            called = true;
        });
        assert(queued.isOk());
        for (int i = 0; i < 100 && !called.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(called.load());
        assert(callbackClose.isClosed());
    }

    {
        lc::HttpClient throwingCallback(config);
        std::atomic<bool> called { false };
        auto queued = throwingCallback.sendAsync(lc::HttpRequest::get("/"), lc::HttpRequestOptions {}, [&](lc::HttpResult) {
            called = true;
            throw std::runtime_error("callback failed");
        });
        assert(queued.isOk());
        for (int i = 0; i < 100 && !called.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(called.load());
        assert(throwingCallback.close().isOk());
    }

    {
        lc::HttpClient owned(config);
        auto provider = owned.authorizationProvider();
        assert(provider);
        assert(owned.close().isOk());
        auto request = lc::HttpRequest::get("/");
        assert(provider->authorize(request).isOk());
    }

#ifndef CPPHTTPLIB_SSL_ENABLED
    {
        auto tlsConfig = config;
        tlsConfig.useTls_ = true;
        lc::HttpClient tlsClient(tlsConfig);
        auto result = tlsClient.send(lc::HttpRequest::get("/"), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::FailedPrecondition);
    }
#endif

    {
        auto logger = std::make_shared<MemoryLogger>();
        auto metrics = std::make_shared<lc::InMemoryMetricRecorder>();
        auto traceSink = std::make_shared<lc::InMemoryTraceSink>();
        lc::DefaultHttpClientFactory factory(lc::HttpClientFactoryOptions {
            .logger_ = logger,
            .metrics_ = metrics,
            .traceSink_ = traceSink,
        });

        auto badConfig = config;
        badConfig.host_.clear();
        auto badClient = factory.create(badConfig, nullptr);
        assert(!badClient.isOk());
        assert(badClient.status().code() == lc::StatusCode::InvalidArgument);

        auto created = factory.create(config, nullptr);
        assert(created.isOk());
        auto failed = (*created)->send(lc::HttpRequest::get("/"), lc::HttpRequestOptions {});
        assert(!failed.isOk());

        auto snapshots = metrics->snapshots();
        bool sawRequestCounter = false;
        bool sawDuration = false;
        for (const auto& snapshot : snapshots) {
            if (snapshot.name_ == "http.client.requests")
                sawRequestCounter = true;
            if (snapshot.name_ == "http.client.duration")
                sawDuration = true;
        }
        assert(sawRequestCounter);
        assert(sawDuration);

        const auto spans = traceSink->spans();
        assert(spans.size() == 1);
        assert(spans.front().name_ == "http.client.request");
        assert(spans.front().status_ == lc::SpanStatus::Error);
        assert(spans.front().attributes_["http.method"] == "GET");
        assert((*created)->close().isOk());
    }

    std::atomic<int> retryHits { 0 };
    std::mutex slowMutex;
    std::condition_variable slowCv;
    bool slowStarted = false;
    std::mutex connectionMutex;
    std::vector<int> connectionPorts;

    LocalHttpServer local([&] {
        auto server = std::make_unique<httplib::Server>();
        server->Get("/ok", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok", "text/plain");
        });
        server->Get("/auth", [](const httplib::Request& req, httplib::Response& res) {
            std::string body;
            body += "authorization=" + req.get_header_value("Authorization") + "\n";
            body += "x-api-key=" + req.get_header_value("X-Api-Key") + "\n";
            body += "x-custom-auth=" + req.get_header_value("X-Custom-Auth") + "\n";
            res.set_content(std::move(body), "text/plain");
        });
        server->Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(req.body, "text/plain");
        });
        server->Get("/redirect", [](const httplib::Request&, httplib::Response& res) {
            res.status = 302;
            res.set_header("Location", "/ok");
        });
        server->Get("/retry", [&](const httplib::Request&, httplib::Response& res) {
            const auto hit = ++retryHits;
            if (hit == 1) {
                res.status = 503;
                res.set_content("try again", "text/plain");
                return;
            }
            res.set_content("retried", "text/plain");
        });
        server->Get("/fail", [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;
            res.set_content("failed", "text/plain");
        });
        server->Get("/large", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(std::string(128, 'x'), "text/plain");
        });
        server->Get("/stream", [](const httplib::Request&, httplib::Response& res) {
            auto chunks = std::make_shared<std::vector<std::string>>(
                std::vector<std::string> { "one", "two", "tri" });
            res.set_chunked_content_provider("text/plain", [chunks](std::size_t offset, httplib::DataSink& sink) {
                const std::size_t index = offset / 3;
                if (index >= chunks->size()) {
                    sink.done();
                    return true;
                }
                const auto& chunk = (*chunks)[index];
                return sink.write(chunk.data(), chunk.size());
            });
        });
        server->Get("/sse", [](const httplib::Request&, httplib::Response& res) {
            auto chunks = std::make_shared<std::vector<std::string>>(
                std::vector<std::string> {
                    "event: token\n",
                    "data: hel",
                    "lo\nid: evt-1\n\n",
                    ": ignored\n",
                    "data: done\nretry: 25\n\n",
                });
            res.set_chunked_content_provider(
                "text/event-stream",
                [chunks, index = std::size_t { 0 }](std::size_t, httplib::DataSink& sink) mutable {
                    if (index >= chunks->size()) {
                        sink.done();
                        return true;
                    }
                    const auto& chunk = (*chunks)[index++];
                    return sink.write(chunk.data(), chunk.size());
                });
        });
        server->Get("/conn", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lock(connectionMutex);
                connectionPorts.push_back(req.remote_port);
            }
            res.set_content("conn", "text/plain");
        });
        server->Get("/slow", [&](const httplib::Request&, httplib::Response& res) {
            {
                std::lock_guard lock(slowMutex);
                slowStarted = true;
            }
            slowCv.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            res.set_content("slow", "text/plain");
        });
        return server;
    });

    if (!local.available())
        return 0;

    lc::HttpClientConfig localConfig;
    localConfig.host_ = "127.0.0.1";
    localConfig.port_ = static_cast<std::uint16_t>(local.port());
    localConfig.connectTimeout_ = std::chrono::milliseconds(500);
    localConfig.readTimeout_ = std::chrono::milliseconds(1000);
    localConfig.writeTimeout_ = std::chrono::milliseconds(500);
    localConfig.closeTimeout_ = std::chrono::milliseconds(250);

    {
        lc::HttpClient real(localConfig);
        auto result = real.send(lc::HttpRequest::get("/ok"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->statusCode_ == 200);
        assert(result->body_ == "ok");
    }

    {
        lc::HttpClient real(localConfig);
        auto result = real.send(lc::HttpRequest::post("/echo", "hello", "text/plain"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->statusCode_ == 200);
        assert(result->body_ == "hello");
    }

    {
        lc::HttpClient real(localConfig);
        auto result = real.send(lc::HttpRequest::get("/redirect"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->statusCode_ == 302);

        auto followConfig = localConfig;
        followConfig.followRedirects_ = true;
        lc::HttpClient following(followConfig);
        auto followed = following.send(lc::HttpRequest::get("/redirect"), lc::HttpRequestOptions {});
        assert(followed.isOk());
        assert(followed->statusCode_ == 200);
        assert(followed->body_ == "ok");
    }

    {
        auto limitedConfig = localConfig;
        limitedConfig.requestLimits_.maxRequestBodyBytes_ = 4;
        lc::HttpClient real(limitedConfig);
        auto result = real.send(lc::HttpRequest::post("/echo", "12345", "text/plain"), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
        auto limitedConfig = localConfig;
        limitedConfig.maxResponseBodyBytes_ = 8;
        lc::HttpClient real(limitedConfig);
        auto result = real.send(lc::HttpRequest::get("/large"), lc::HttpRequestOptions {});
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
        lc::HttpClient real(localConfig);
        std::string streamed;
        auto result = real.sendStreaming(
            lc::HttpRequest::get("/stream"),
            lc::HttpRequestOptions {},
            [&](std::string_view chunk) {
                streamed.append(chunk);
                return lc::Status::ok();
            },
            lc::HttpStreamOptions { .chunkBytes_ = 3 });
        assert(result.isOk());
        assert(result->statusCode_ == 200);
        assert(result->body_.empty());
        assert(streamed == "onetwotri");
    }

    {
        auto limitedConfig = localConfig;
        limitedConfig.maxResponseBodyBytes_ = 6;
        lc::HttpClient real(limitedConfig);
        auto result = real.sendStreaming(
            lc::HttpRequest::get("/stream"),
            lc::HttpRequestOptions {},
            [](std::string_view) {
                return lc::Status::ok();
            },
            lc::HttpStreamOptions { .chunkBytes_ = 3 });
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
        lc::HttpClient real(localConfig);
        std::vector<lc::ServerSentEvent> events;
        auto result = real.sendSse(
            lc::HttpRequest::get("/sse"),
            lc::HttpRequestOptions {},
            [&](const lc::ServerSentEvent& event) {
                events.push_back(event);
                return lc::Status::ok();
            },
            lc::HttpStreamOptions { .chunkBytes_ = 4 });
        assert(result.isOk());
        assert(result->statusCode_ == 200);
        assert(events.size() == 2);
        assert(events[0].event_ == "token");
        assert(events[0].data_ == "hello");
        assert(events[0].id_ == "evt-1");
        assert(events[1].event_ == "message");
        assert(events[1].data_ == "done");
        assert(events[1].retry_.has_value());
        assert(*events[1].retry_ == std::chrono::milliseconds(25));
    }

    {
        lc::HttpClient real(localConfig);
        auto result = real.sendSse(
            lc::HttpRequest::get("/sse"),
            lc::HttpRequestOptions {},
            [](const lc::ServerSentEvent&) {
                return lc::Status::cancelled("stop streaming");
            });
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::Cancelled);
    }

    {
        auto retryConfig = localConfig;
        retryConfig.retryPolicy_.maxRetries_ = 1;
        retryConfig.retryPolicy_.delay_ = std::chrono::milliseconds(1);
        lc::HttpClient real(retryConfig);
        retryHits.store(0);
        auto result = real.send(lc::HttpRequest::get("/retry"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->statusCode_ == 200);
        assert(result->body_ == "retried");
        assert(retryHits.load() == 2);
    }

    {
        lc::HttpClient real(localConfig);
        retryHits.store(0);
        auto result = real.send(
            lc::HttpRequest::get("/retry"),
            lc::HttpRequestOptions {
                .retryPolicy_ = lc::HttpRetryPolicy {
                    .maxRetries_ = 1,
                    .delay_ = std::chrono::milliseconds(1),
                },
            });
        assert(result.isOk());
        assert(result->statusCode_ == 200);
        assert(result->body_ == "retried");
        assert(retryHits.load() == 2);
    }

    {
        auto retryConfig = localConfig;
        retryConfig.retryPolicy_.maxRetries_ = 1;
        retryConfig.retryPolicy_.delay_ = std::chrono::milliseconds(1);
        lc::HttpClient real(retryConfig);
        retryHits.store(0);
        auto result = real.send(
            lc::HttpRequest::get("/retry"),
            lc::HttpRequestOptions {
                .retryPolicy_ = lc::HttpRetryPolicy {
                    .maxRetries_ = 0,
                },
            });
        assert(result.isOk());
        assert(result->statusCode_ == 503);
        assert(retryHits.load() == 1);
    }

    {
        lc::HttpClient real(localConfig);
        auto result = real.send(
            lc::HttpRequest::get("/slow"),
            lc::HttpRequestOptions {
                .timeout_ = std::chrono::milliseconds(20),
            });
        assert(!result.isOk());
        assert(result.status().code() == lc::StatusCode::DeadlineExceeded);
    }

    {
        {
            std::lock_guard lock(connectionMutex);
            connectionPorts.clear();
        }
        auto pooledConfig = localConfig;
        pooledConfig.keepAlive_ = true;
        pooledConfig.connectionPoolSize_ = 1;
        lc::HttpClient real(pooledConfig);
        for (int i = 0; i < 3; ++i) {
            auto result = real.send(lc::HttpRequest::get("/conn"), lc::HttpRequestOptions {});
            assert(result.isOk());
            assert(result->body_ == "conn");
        }
        std::set<int> uniquePorts;
        {
            std::lock_guard lock(connectionMutex);
            uniquePorts.insert(connectionPorts.begin(), connectionPorts.end());
        }
        assert(uniquePorts.size() == 1);
    }

    {
        auto auth = std::make_shared<lc::BearerTokenAuthorizationProvider>("request-token");
        lc::HttpClient real(localConfig, auth);
        auto result = real.send(lc::HttpRequest::get("/auth"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->body_.find("authorization=Bearer request-token") != std::string::npos);
    }

    {
        auto auth = std::make_shared<lc::ApiKeyAuthorizationProvider>("X-Api-Key", "api-key-value");
        lc::HttpClient real(localConfig, auth);
        auto result = real.send(lc::HttpRequest::get("/auth"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->body_.find("x-api-key=api-key-value") != std::string::npos);
    }

    {
        auto auth = std::make_shared<lc::BasicAuthorizationProvider>("user", "pass");
        lc::HttpClient real(localConfig, auth);
        auto result = real.send(lc::HttpRequest::get("/auth"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->body_.find("authorization=Basic dXNlcjpwYXNz") != std::string::npos);
    }

    {
        auto auth = std::make_shared<lc::FunctionAuthorizationProvider>([](lc::HttpRequest& request) {
            request.setHeader("X-Custom-Auth", "signed");
            return lc::Status::ok();
        });
        lc::HttpClient real(localConfig, auth);
        auto result = real.send(lc::HttpRequest::get("/auth"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->body_.find("x-custom-auth=signed") != std::string::npos);
    }

    {
        auto logger = std::make_shared<MemoryLogger>();
        auto auth = std::make_shared<lc::OAuthAuthorizationProvider>();
        assert(auth->apply(lc::OAuthCredentials {
            .accessToken_ = "secret-token-1234567890",
            .accessTokenExpiresAt_ = std::chrono::system_clock::now() + std::chrono::hours(1),
        }).isOk());
        lc::HttpClient real(localConfig, auth, logger);
        auto request = lc::HttpRequest::post("/ok", R"({"api_key":"sk-secret-1234567890"})");
        auto result = real.send(std::move(request), lc::HttpRequestOptions {});
        assert(result.isOk());
        const auto logText = logger->text();
        assert(logText.find("secret-token-1234567890") == std::string::npos);
        assert(logText.find("sk-secret-1234567890") == std::string::npos);
        assert(logText.find("<omitted bytes=") != std::string::npos);
    }

    {
        auto logger = std::make_shared<MemoryLogger>();
        lc::DefaultHttpClientFactory factory(lc::HttpClientFactoryOptions {
            .logger_ = logger,
        });
        auto auth = std::make_shared<lc::OAuthAuthorizationProvider>();
        assert(auth->apply(lc::OAuthCredentials {
            .accessToken_ = "factory-secret-token-1234567890",
            .accessTokenExpiresAt_ = std::chrono::system_clock::now() + std::chrono::hours(1),
        }).isOk());

        auto created = factory.create(localConfig, auth);
        assert(created.isOk());
        auto request = lc::HttpRequest::post("/ok", R"({"api_key":"sk-factory-secret-1234567890"})");
        auto result = (*created)->send(std::move(request), lc::HttpRequestOptions {});
        assert(result.isOk());
        const auto logText = logger->text();
        assert(logText.find("factory-secret-token-1234567890") == std::string::npos);
        assert(logText.find("sk-factory-secret-1234567890") == std::string::npos);
        assert(logText.find("[REDACTED]") != std::string::npos || logText.find("<omitted bytes=") != std::string::npos);
        assert((*created)->close().isOk());
    }

    {
        auto rateConfig = localConfig;
        rateConfig.rateLimiter_ = std::make_shared<lc::TokenBucketRateLimiter>(lc::RateLimitPolicy {
            .capacity_ = 1,
            .refill_ = 1,
            .interval_ = std::chrono::hours(1),
        });
        lc::HttpClient real(rateConfig);
        assert(real.send(lc::HttpRequest::get("/ok"), lc::HttpRequestOptions {}).isOk());
        auto limited = real.send(lc::HttpRequest::get("/ok"), lc::HttpRequestOptions {});
        assert(!limited.isOk());
        assert(limited.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
        auto circuitConfig = localConfig;
        circuitConfig.circuitBreaker_ = std::make_shared<lc::CircuitBreaker>(lc::CircuitBreakerPolicy {
            .failureThreshold_ = 1,
            .successThreshold_ = 1,
            .halfOpenMaxCalls_ = 1,
            .openTimeout_ = std::chrono::hours(1),
        });
        lc::HttpClient real(circuitConfig);
        auto failed = real.send(lc::HttpRequest::get("/fail"), lc::HttpRequestOptions {});
        assert(failed.isOk());
        assert(failed->statusCode_ == 500);
        auto blocked = real.send(lc::HttpRequest::get("/ok"), lc::HttpRequestOptions {});
        assert(!blocked.isOk());
        assert(blocked.status().code() == lc::StatusCode::Unavailable);
    }

    std::atomic<int> proxyHits { 0 };
    LocalHttpServer proxy([&] {
        auto server = std::make_unique<httplib::Server>();
        server->Get(R"(.+)", [&](const httplib::Request& req, httplib::Response& res) {
            ++proxyHits;
            res.set_content(std::string("proxied:") + req.path, "text/plain");
        });
        return server;
    });

    if (proxy.available()) {
        auto proxyClientConfig = localConfig;
        proxyClientConfig.host_ = "example.invalid";
        proxyClientConfig.port_ = 80;
        proxyClientConfig.proxy_.host_ = "127.0.0.1";
        proxyClientConfig.proxy_.port_ = static_cast<std::uint16_t>(proxy.port());
        lc::HttpClient real(proxyClientConfig);
        auto result = real.send(lc::HttpRequest::get("/proxy-ok"), lc::HttpRequestOptions {});
        assert(result.isOk());
        assert(result->statusCode_ == 200);
        assert(result->body_.find("proxied:") == 0);
        assert(proxyHits.load() == 1);
    }

#ifdef CPPHTTPLIB_SSL_ENABLED
    auto tlsMaterial = std::make_shared<TestTlsMaterial>();
    if (tlsMaterial->valid()) {
        LocalHttpServer tlsLocal([&] {
            auto server = std::make_unique<httplib::SSLServer>(tlsMaterial->pem());
            server->Get("/tls", [](const httplib::Request&, httplib::Response& res) {
                res.set_content("secure", "text/plain");
            });
            return server;
        });

        if (tlsLocal.available()) {
            auto tlsConfig = localConfig;
            tlsConfig.port_ = static_cast<std::uint16_t>(tlsLocal.port());
            tlsConfig.useTls_ = true;
            tlsConfig.tlsOptions_.verifyPeer_ = false;
            lc::HttpClient tlsClient(tlsConfig);
            auto result = tlsClient.send(lc::HttpRequest::get("/tls"), lc::HttpRequestOptions {});
            assert(result.isOk());
            assert(result->statusCode_ == 200);
            assert(result->body_ == "secure");

            auto verifiedConfig = tlsConfig;
            verifiedConfig.tlsOptions_.verifyPeer_ = true;
            lc::HttpClient verified(verifiedConfig);
            auto rejected = verified.send(lc::HttpRequest::get("/tls"), lc::HttpRequestOptions {});
            assert(!rejected.isOk());
        }
    }
#endif

    {
        lc::HttpClient real(localConfig);
        std::atomic<bool> callbackCalled { false };
        {
            std::lock_guard lock(slowMutex);
            slowStarted = false;
        }
        assert(real.sendAsync(lc::HttpRequest::get("/slow"), lc::HttpRequestOptions {}, [&](lc::HttpResult result) {
            callbackCalled = true;
            assert(!result.isOk() || result->statusCode_ == 200);
        }).isOk());

        {
            std::unique_lock lock(slowMutex);
            slowCv.wait_for(lock, std::chrono::seconds(1), [&] { return slowStarted; });
        }
        assert(slowStarted);
        assert(real.close().isOk());
        for (int i = 0; i < 100 && !callbackCalled.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(callbackCalled.load());
    }

    return 0;
}
