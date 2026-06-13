#pragma once

#include "foundation/network/i_http_client.hpp"
#include "foundation/network/i_oauth2_token_provider.hpp"

#include <memory>

namespace lc {

/// Outbound HTTP/HTTPS client: libcpr (libcurl) + serial async queue + token gate.
class HttpClient final : public IHttpClient {
public:
    explicit HttpClient(HttpClientConfig config);

    /// Injects a custom `IOAuth2TokenProvider` (tests, vault-backed credentials). If null, builds a
    /// default `OAuth2TokenProvider` (`OAuth2TokenProvider::kDefaultRenewLead`). Prefer this over a
    /// runtime setter so the worker always sees a stable provider + wired `onApply`.
    HttpClient(HttpClientConfig config, std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider);

    ~HttpClient() override;

    void request(std::shared_ptr<HttpRequestSpec> requestSpec, HttpAsyncCallback callback) override;

    [[nodiscard]] std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider() const override;

    void shutdown() noexcept override;

public:
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lc
