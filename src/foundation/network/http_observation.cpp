#include "foundation/network/http_observation.hh"

#include "foundation/observability/metrics.hpp"
#include "foundation/observability/tracing.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace lgc::http_client_detail {

HttpObservation::HttpObservation(
    std::shared_ptr<IMetricRecorder> metrics,
    std::shared_ptr<ITraceSink> traceSink,
    HttpMethod method,
    std::string host,
    std::uint16_t port,
    std::string redactedUrl)
    : metrics_(std::move(metrics))
    , traceSink_(std::move(traceSink))
    , startedAt_(SteadyClock::instance().now())
    , method_(method)
    , host_(std::move(host))
    , port_(port)
    , redactedUrl_(std::move(redactedUrl))
{
}

HttpObservation::~HttpObservation() noexcept
{
    finish();
}

void HttpObservation::response(int statusCode, std::size_t responseBytes) noexcept
{
    statusCode_ = statusCode;
    responseBytes_ = responseBytes;
    status_ = Status::ok();
}

void HttpObservation::error(Status status) noexcept
{
    status_ = std::move(status);
    statusCode_ = 0;
    responseBytes_ = 0;
}

void HttpObservation::finish() noexcept
{
    if (finished_)
        return;
    finished_ = true;

    try {
        finishUnsafe();
    } catch (...) {
    }
}

void HttpObservation::finishUnsafe()
{
    const auto endedAt = SteadyClock::instance().now();
    const auto duration = endedAt - startedAt_;
    const std::string method(httpMethodName(method_));
    const bool httpError = status_.isOk() && statusCode_ >= 500;
    const bool ok = status_.isOk() && !httpError;

    MetricTags tags {
        { "http.method", method },
        { "server.address", host_ },
        { "outcome", ok ? "ok" : "error" },
    };
    if (statusCode_ > 0)
        tags.push_back({ "http.status_code", std::to_string(statusCode_) });
    if (!status_.isOk())
        tags.push_back({ "error.code", std::string(status_.codeName()) });

    if (metrics_) {
        (void)metrics_->incrementCounter("http.client.requests", 1.0, tags);
        (void)metrics_->recordDuration("http.client.duration", duration, tags);
        if (statusCode_ > 0)
            (void)metrics_->recordHistogram(
                "http.client.response.bytes",
                static_cast<double>(responseBytes_),
                tags);
    }

    if (traceSink_) {
        nlohmann::json attributes {
            { "http.method", method },
            { "url.full", redactedUrl_ },
            { "server.address", host_ },
            { "server.port", port_ },
        };
        if (statusCode_ > 0)
            attributes["http.response.status_code"] = statusCode_;
        if (!status_.isOk())
            attributes["error.code"] = std::string(status_.codeName());

        auto context = makeRootContext();
        if (context.isOk()) {
            SpanRecord span {
                .context_ = std::move(*context),
                .name_ = "http.client.request",
                .attributes_ = std::move(attributes),
                .startedAt_ = startedAt_,
                .endedAt_ = endedAt,
                .status_ = ok ? SpanStatus::Ok : SpanStatus::Error,
                .statusMessage_ = status_.isOk() ? std::string() : status_.toString(),
            };
            (void)traceSink_->recordSpan(std::move(span));
        }
    }
}

} // namespace lgc::http_client_detail
