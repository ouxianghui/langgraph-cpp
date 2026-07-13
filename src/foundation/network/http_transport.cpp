#include "foundation/network/http_transport.hh"

#include "foundation/network/http_client_common.hh"
#include "foundation/redaction/redactor.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace lc::http_client_detail {
namespace {

std::string lowerAscii(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::optional<std::size_t> parseContentLengthHeader(const httplib::Headers& hdr)
{
    for (const auto& kv : hdr) {
        if (lowerAscii(kv.first) != "content-length")
            continue;
        const std::string_view val = trimView(kv.second);
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

} // namespace

std::string formatHeadersForLog(
    const httplib::Headers& hdr,
    const HttpLogOptions& options,
    const Redactor& redactor)
{
    if (!options.logHeaders_)
        return "<omitted>\n";

    std::ostringstream oss;
    for (const auto& kv : hdr) {
        const auto value = redactor.sensitiveKey(kv.first)
            ? redactor.config().replacement_
            : redactor.redact(kv.second);
        oss << kv.first << ": " << value << '\n';
    }
    return oss.str();
}

std::string formatBodyForLog(
    const std::string& body,
    const HttpLogOptions& options,
    const Redactor& redactor)
{
    if (body.empty())
        return {};
    if (!options.logBodies_)
        return "<omitted bytes=" + std::to_string(body.size()) + ">";

    std::string text;
    if (options.maxBodyBytes_ > 0 && body.size() > options.maxBodyBytes_) {
        text.assign(body.data(), options.maxBodyBytes_);
        text.append("...<truncated bytes=");
        text.append(std::to_string(body.size() - options.maxBodyBytes_));
        text.push_back('>');
    } else {
        text = body;
    }
    return redactor.redact(text);
}

Status statusFromHttplibError(httplib::Error err)
{
    switch (err) {
    case httplib::Error::Success:
        return Status::ok();
    case httplib::Error::ConnectionTimeout:
    case httplib::Error::Timeout:
        return Status::deadlineExceeded("HTTP request timed out");
    case httplib::Error::ConnectionClosed:
        return Status::unavailable("HTTP connection closed before response");
    case httplib::Error::ExceedMaxPayloadSize:
        return Status::resourceExhausted("HTTP response body exceeds maxResponseBodyBytes");
    case httplib::Error::InvalidHTTPMethod:
    case httplib::Error::InvalidHTTPVersion:
    case httplib::Error::InvalidHeaders:
    case httplib::Error::InvalidRequestLine:
    case httplib::Error::HTTPParsing:
    case httplib::Error::InvalidRangeHeader:
        return Status::internal("HTTP protocol error: " + httplib::to_string(err));
    default:
        return Status::unavailable("HTTP transport error: " + httplib::to_string(err));
    }
}

Status statusFromHttplibResultFailure(httplib::Error err, std::size_t maxResponseBodyBytes)
{
    // cpp-httplib may surface a payload-limit read abort as Error::Read.
    if (err == httplib::Error::Read && maxResponseBodyBytes > 0) {
        return Status::resourceExhausted("HTTP response body exceeds maxResponseBodyBytes");
    }
    return statusFromHttplibError(err);
}

Result<HttpResponse> buildHttpResponse(const httplib::Response& r, std::size_t maxBodyBytes)
{
    HttpResponse response;
    response.statusCode_ = r.status;
    response.reason_ = r.reason;
    for (const auto& kv : r.headers) {
        response.headers_.emplace_back(kv.first, kv.second);
    }

    if (maxBodyBytes > 0) {
        if (const auto cl = parseContentLengthHeader(r.headers);
            cl.has_value() && *cl > maxBodyBytes) {
            return Status::resourceExhausted("HTTP Content-Length exceeds maxResponseBodyBytes");
        }
    }

    if (maxBodyBytes > 0 && r.body.size() > maxBodyBytes) {
        return Status::resourceExhausted("HTTP response body exceeds maxResponseBodyBytes");
    }

    response.body_ = r.body;
    return response;
}

Result<HttpResponse> buildStreamingHttpResponse(
    const httplib::stream::Result& r,
    std::size_t maxBodyBytes)
{
    HttpResponse response;
    response.statusCode_ = r.status();
    for (const auto& kv : r.headers()) {
        response.headers_.emplace_back(kv.first, kv.second);
    }

    if (maxBodyBytes > 0) {
        if (const auto cl = parseContentLengthHeader(r.headers());
            cl.has_value() && *cl > maxBodyBytes) {
            return Status::resourceExhausted("HTTP Content-Length exceeds maxResponseBodyBytes");
        }
    }

    return response;
}

httplib::Headers buildRequestHeaders(const HttpClientConfig& cfg, const HttpRequest& request)
{
    httplib::Headers hdr;
    if (!cfg.userAgent_.empty()) {
        hdr.emplace("User-Agent", cfg.userAgent_);
    }
    for (const auto& kv : request.headers_) {
        if (httpHeaderNameEquals(kv.first, "host"))
            continue;
        hdr.emplace(kv.first, kv.second);
    }
    if (!request.body_.empty() && request.contentType_.has_value()) {
        hdr.emplace("Content-Type", *request.contentType_);
    }
    return hdr;
}

httplib::Result performRequest(httplib::Client& client,
    HttpMethod method,
    const std::string& path,
    const httplib::Headers& headers,
    const HttpRequest& request)
{
    const std::string contentType = request.contentType_.value_or(std::string());
    switch (method) {
    case HttpMethod::Get:
        return client.Get(path, headers);
    case HttpMethod::Head:
        return client.Head(path, headers);
    case HttpMethod::Post:
        if (request.body_.empty())
            return client.Post(path, headers);
        return client.Post(path, headers, request.body_, contentType);
    case HttpMethod::Put:
        if (request.body_.empty())
            return client.Put(path, headers);
        return client.Put(path, headers, request.body_, contentType);
    case HttpMethod::Delete:
        if (request.body_.empty())
            return client.Delete(path, headers);
        return client.Delete(path, headers, request.body_, contentType);
    case HttpMethod::Options:
        return client.Options(path, headers);
    case HttpMethod::Patch:
        if (request.body_.empty())
            return client.Patch(path, headers);
        return client.Patch(path, headers, request.body_, contentType);
    }
    return {};
}

httplib::stream::Result performStreamingRequest(httplib::Client& client,
    HttpMethod method,
    const std::string& path,
    const httplib::Headers& headers,
    const HttpRequest& request,
    const HttpStreamOptions& options)
{
    const std::size_t chunkBytes = options.chunkBytes_ == 0 ? 8192 : options.chunkBytes_;
    return httplib::stream::Result(
        client.open_stream(
            std::string(httpMethodName(method)),
            path,
            httplib::Params {},
            headers,
            request.body_,
            request.contentType_.value_or(std::string())),
        chunkBytes);
}

void configureClient(httplib::Client& client, const HttpClientConfig& cfg)
{
    client.set_keep_alive(cfg.keepAlive_);
    client.set_follow_location(cfg.followRedirects_);
    if (cfg.proxy_.isEnabled()) {
        client.set_proxy(cfg.proxy_.host_, static_cast<int>(cfg.proxy_.port_));
        switch (cfg.proxy_.auth_) {
        case HttpProxyAuth::None:
            break;
        case HttpProxyAuth::Basic:
            client.set_proxy_basic_auth(cfg.proxy_.username_, cfg.proxy_.password_);
            break;
        case HttpProxyAuth::Bearer:
            client.set_proxy_bearer_token_auth(cfg.proxy_.bearerToken_);
            break;
        }
    }
    if (cfg.maxResponseBodyBytes_ > 0) {
        client.set_payload_max_length(cfg.maxResponseBodyBytes_);
    }

    if (cfg.connectTimeout_ > std::chrono::milliseconds::zero()) {
        client.set_connection_timeout(cfg.connectTimeout_);
    }
    if (cfg.readTimeout_ > std::chrono::milliseconds::zero()) {
        client.set_read_timeout(cfg.readTimeout_);
    }
    if (cfg.writeTimeout_ > std::chrono::milliseconds::zero()) {
        client.set_write_timeout(cfg.writeTimeout_);
    }

#ifdef CPPHTTPLIB_SSL_ENABLED
    if (cfg.useTls_) {
        client.enable_server_certificate_verification(cfg.tlsOptions_.verifyPeer_);
        client.enable_server_hostname_verification(cfg.tlsOptions_.verifyPeer_);
        if (cfg.tlsOptions_.minVersion_ != TlsMinVersion::Default) {
            const auto version = cfg.tlsOptions_.minVersion_ == TlsMinVersion::Tls13
                ? httplib::tls::Version::TLS1_3
                : httplib::tls::Version::TLS1_2;
            (void)httplib::tls::set_min_version(client.tls_context(), version);
        }
        if (!cfg.tlsOptions_.caBundleFile_.empty()
            || !cfg.tlsOptions_.caPath_.empty()) {
            client.set_ca_cert_path(
                cfg.tlsOptions_.caBundleFile_,
                cfg.tlsOptions_.caPath_);
        }
    }
#else
    (void)cfg;
#endif
}

} // namespace lc::http_client_detail
