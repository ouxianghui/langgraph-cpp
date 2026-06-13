#pragma once

#include "foundation/network/http_client_types.hpp"

#include <memory>

namespace lc {

class IHttpClient;
class IOAuth2TokenProvider;

class IHttpClientFactory {
public:
    virtual ~IHttpClientFactory() = default;

    [[nodiscard]] virtual std::shared_ptr<IHttpClient> create(
        HttpClientConfig config,
        std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider)
        = 0;
};

class DefaultHttpClientFactory final : public IHttpClientFactory {
public:
    [[nodiscard]] std::shared_ptr<IHttpClient> create(
        HttpClientConfig config,
        std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider) override;
};

[[nodiscard]] std::shared_ptr<IHttpClientFactory> defaultHttpClientFactory();

} // namespace lc
