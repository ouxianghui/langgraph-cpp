#include "foundation/network/sse_parser.hh"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>

namespace lgc::http_client_detail {

Status SseParser::feed(std::string_view chunk, const ServerSentEventCallback& callback)
{
    buffer_.append(chunk.data(), chunk.size());
    for (;;) {
        const auto pos = buffer_.find('\n');
        if (pos == std::string::npos)
            return Status::ok();

        std::string line = buffer_.substr(0, pos);
        buffer_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (auto status = consumeLine(line, callback); !status.isOk())
            return status;
    }
}

Status SseParser::finish(const ServerSentEventCallback& callback)
{
    if (!buffer_.empty()) {
        std::string line = std::move(buffer_);
        buffer_.clear();
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (auto status = consumeLine(line, callback); !status.isOk())
            return status;
    }
    return dispatch(callback);
}

Status SseParser::consumeLine(
    const std::string& line,
    const ServerSentEventCallback& callback)
{
    if (line.empty())
        return dispatch(callback);
    if (!line.empty() && line.front() == ':')
        return Status::ok();

    const auto colon = line.find(':');
    std::string_view field(line.data(), colon == std::string::npos ? line.size() : colon);
    std::string_view value;
    if (colon != std::string::npos) {
        value = std::string_view(line.data() + colon + 1, line.size() - colon - 1);
        if (!value.empty() && value.front() == ' ')
            value.remove_prefix(1);
    }

    if (field == "event") {
        event_ = std::string(value);
    } else if (field == "data") {
        if (hasData_)
            data_.push_back('\n');
        data_.append(value.data(), value.size());
        hasData_ = true;
    } else if (field == "id") {
        id_ = std::string(value);
    } else if (field == "retry") {
        std::uint64_t parsed = 0;
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec == std::errc {} && result.ptr == end
            && parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            retry_ = std::chrono::milliseconds(parsed);
        }
    }
    return Status::ok();
}

Status SseParser::dispatch(const ServerSentEventCallback& callback)
{
    if (!hasData_) {
        reset();
        return Status::ok();
    }

    ServerSentEvent event {
        .event_ = event_.empty() ? "message" : event_,
        .data_ = data_,
        .id_ = id_,
        .retry_ = retry_,
    };
    reset();

    try {
        return callback(event);
    } catch (const std::exception& ex) {
        return Status::unknown(std::string("SSE callback failed: ") + ex.what());
    } catch (...) {
        return Status::unknown("SSE callback failed");
    }
}

void SseParser::reset()
{
    event_.clear();
    data_.clear();
    id_.clear();
    retry_.reset();
    hasData_ = false;
}

} // namespace lgc::http_client_detail
