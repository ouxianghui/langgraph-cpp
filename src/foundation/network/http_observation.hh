#pragma once

#include "foundation/network/http_client_types.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lgc {
class IMetricRecorder;
class ITraceSink;
}

namespace lgc::http_client_detail {

class HttpObservation final {
public:
    HttpObservation(
        std::shared_ptr<IMetricRecorder> metrics,
        std::shared_ptr<ITraceSink> traceSink,
        HttpMethod method,
        std::string host,
        std::uint16_t port,
        std::string redactedUrl);
    ~HttpObservation() noexcept;

    HttpObservation(const HttpObservation&) = delete;
    HttpObservation& operator=(const HttpObservation&) = delete;

    void response(int statusCode, std::size_t responseBytes) noexcept;
    void error(Status status) noexcept;
    void finish() noexcept;

private:
    void finishUnsafe();

    std::shared_ptr<IMetricRecorder> metrics_;
    std::shared_ptr<ITraceSink> traceSink_;
    Clock::TimePoint startedAt_;
    HttpMethod method_;
    std::string host_;
    std::uint16_t port_ { 0 };
    std::string redactedUrl_;
    Status status_;
    int statusCode_ { 0 };
    std::size_t responseBytes_ { 0 };
    bool finished_ { false };
};

} // namespace lgc::http_client_detail
