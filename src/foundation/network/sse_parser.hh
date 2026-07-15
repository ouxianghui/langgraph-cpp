#pragma once

#include "foundation/network/http_client_types.hpp"
#include "foundation/status/status.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace lgc::http_client_detail {

class SseParser final {
public:
    [[nodiscard]] Status feed(std::string_view chunk, const ServerSentEventCallback& callback);
    [[nodiscard]] Status finish(const ServerSentEventCallback& callback);

private:
    [[nodiscard]] Status consumeLine(
        const std::string& line,
        const ServerSentEventCallback& callback);
    [[nodiscard]] Status dispatch(const ServerSentEventCallback& callback);
    void reset();

    std::string buffer_;
    std::string event_;
    std::string data_;
    std::string id_;
    std::optional<std::chrono::milliseconds> retry_;
    bool hasData_ { false };
};

} // namespace lgc::http_client_detail
