#include "foundation/network/http_client_factory.hpp"

#include "foundation/network/http_client.hpp"

#include <utility>

namespace lc {

std::shared_ptr<IHttpClient> DefaultHttpClientFactory::create(
    HttpClientConfig config,
    std::shared_ptr<IOAuth2TokenProvider> oauth2TokenProvider)
{
    return std::make_shared<HttpClient>(std::move(config), std::move(oauth2TokenProvider));
}

std::shared_ptr<IHttpClientFactory> defaultHttpClientFactory()
{
    return std::make_shared<DefaultHttpClientFactory>();
}

} // namespace lc
