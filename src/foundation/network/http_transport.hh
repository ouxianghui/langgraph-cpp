#pragma once

#include "foundation/network/http_client_types.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <httplib.h>

#include <cstddef>
#include <string>

namespace lc {
class Redactor;
}

namespace lc::http_client_detail {

[[nodiscard]] std::string formatHeadersForLog(
    const httplib::Headers& hdr,
    const HttpLogOptions& options,
    const Redactor& redactor);
[[nodiscard]] std::string formatBodyForLog(
    const std::string& body,
    const HttpLogOptions& options,
    const Redactor& redactor);

[[nodiscard]] Status statusFromHttplibError(httplib::Error err);
[[nodiscard]] Status statusFromHttplibResultFailure(httplib::Error err, std::size_t maxResponseBodyBytes);
[[nodiscard]] Result<HttpResponse> buildHttpResponse(const httplib::Response& r, std::size_t maxBodyBytes);
[[nodiscard]] Result<HttpResponse> buildStreamingHttpResponse(
    const httplib::stream::Result& r,
    std::size_t maxBodyBytes);
[[nodiscard]] httplib::Headers buildRequestHeaders(const HttpClientConfig& cfg, const HttpRequest& request);
[[nodiscard]] httplib::Result performRequest(
    httplib::Client& client,
    HttpMethod method,
    const std::string& path,
    const httplib::Headers& headers,
    const HttpRequest& request);
[[nodiscard]] httplib::stream::Result performStreamingRequest(
    httplib::Client& client,
    HttpMethod method,
    const std::string& path,
    const httplib::Headers& headers,
    const HttpRequest& request,
    const HttpStreamOptions& options);
void configureClient(httplib::Client& client, const HttpClientConfig& cfg);

} // namespace lc::http_client_detail
