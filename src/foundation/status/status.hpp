#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace lc {

enum class StatusCode : std::uint8_t {
    Ok = 0,
    Cancelled,
    Unknown,
    InvalidArgument,
    DeadlineExceeded,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    ResourceExhausted,
    FailedPrecondition,
    Aborted,
    OutOfRange,
    Unimplemented,
    Internal,
    Unavailable,
    DataLoss,
    Unauthenticated,
};

[[nodiscard]] constexpr std::string_view statusCodeName(StatusCode code) noexcept
{
    switch (code) {
    case StatusCode::Ok:
        return "ok";
    case StatusCode::Cancelled:
        return "cancelled";
    case StatusCode::Unknown:
        return "unknown";
    case StatusCode::InvalidArgument:
        return "invalid_argument";
    case StatusCode::DeadlineExceeded:
        return "deadline_exceeded";
    case StatusCode::NotFound:
        return "not_found";
    case StatusCode::AlreadyExists:
        return "already_exists";
    case StatusCode::PermissionDenied:
        return "permission_denied";
    case StatusCode::ResourceExhausted:
        return "resource_exhausted";
    case StatusCode::FailedPrecondition:
        return "failed_precondition";
    case StatusCode::Aborted:
        return "aborted";
    case StatusCode::OutOfRange:
        return "out_of_range";
    case StatusCode::Unimplemented:
        return "unimplemented";
    case StatusCode::Internal:
        return "internal";
    case StatusCode::Unavailable:
        return "unavailable";
    case StatusCode::DataLoss:
        return "data_loss";
    case StatusCode::Unauthenticated:
        return "unauthenticated";
    }
    return "unknown";
}

class Status final {
public:
    constexpr Status() noexcept = default;

    explicit Status(StatusCode code)
        : code_(normalizeCode(code))
    {
    }

    Status(StatusCode code, std::string message)
        : code_(normalizeCode(code))
        , message_(code_ == StatusCode::Ok ? std::string() : std::move(message))
    {
    }

    [[nodiscard]] static constexpr Status ok() noexcept { return Status {}; }

    [[nodiscard]] static Status cancelled(std::string message = {})
    {
        return Status(StatusCode::Cancelled, std::move(message));
    }

    [[nodiscard]] static Status unknown(std::string message = {})
    {
        return Status(StatusCode::Unknown, std::move(message));
    }

    [[nodiscard]] static Status invalidArgument(std::string message = {})
    {
        return Status(StatusCode::InvalidArgument, std::move(message));
    }

    [[nodiscard]] static Status deadlineExceeded(std::string message = {})
    {
        return Status(StatusCode::DeadlineExceeded, std::move(message));
    }

    [[nodiscard]] static Status notFound(std::string message = {})
    {
        return Status(StatusCode::NotFound, std::move(message));
    }

    [[nodiscard]] static Status alreadyExists(std::string message = {})
    {
        return Status(StatusCode::AlreadyExists, std::move(message));
    }

    [[nodiscard]] static Status permissionDenied(std::string message = {})
    {
        return Status(StatusCode::PermissionDenied, std::move(message));
    }

    [[nodiscard]] static Status resourceExhausted(std::string message = {})
    {
        return Status(StatusCode::ResourceExhausted, std::move(message));
    }

    [[nodiscard]] static Status failedPrecondition(std::string message = {})
    {
        return Status(StatusCode::FailedPrecondition, std::move(message));
    }

    [[nodiscard]] static Status aborted(std::string message = {})
    {
        return Status(StatusCode::Aborted, std::move(message));
    }

    [[nodiscard]] static Status outOfRange(std::string message = {})
    {
        return Status(StatusCode::OutOfRange, std::move(message));
    }

    [[nodiscard]] static Status unimplemented(std::string message = {})
    {
        return Status(StatusCode::Unimplemented, std::move(message));
    }

    [[nodiscard]] static Status internal(std::string message = {})
    {
        return Status(StatusCode::Internal, std::move(message));
    }

    [[nodiscard]] static Status unavailable(std::string message = {})
    {
        return Status(StatusCode::Unavailable, std::move(message));
    }

    [[nodiscard]] static Status dataLoss(std::string message = {})
    {
        return Status(StatusCode::DataLoss, std::move(message));
    }

    [[nodiscard]] static Status unauthenticated(std::string message = {})
    {
        return Status(StatusCode::Unauthenticated, std::move(message));
    }

    [[nodiscard]] constexpr bool isOk() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return isOk(); }

    [[nodiscard]] constexpr StatusCode code() const noexcept { return code_; }
    [[nodiscard]] constexpr std::string_view codeName() const noexcept { return statusCodeName(code_); }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    [[nodiscard]] std::string toString() const
    {
        if (message_.empty())
            return std::string(codeName());
        std::string out(codeName());
        out.append(": ");
        out.append(message_);
        return out;
    }

    friend bool operator==(const Status&, const Status&) = default;

private:
    [[nodiscard]] static constexpr StatusCode normalizeCode(StatusCode code) noexcept
    {
        return isKnownCode(code) ? code : StatusCode::Unknown;
    }

    [[nodiscard]] static constexpr bool isKnownCode(StatusCode code) noexcept
    {
        switch (code) {
        case StatusCode::Ok:
        case StatusCode::Cancelled:
        case StatusCode::Unknown:
        case StatusCode::InvalidArgument:
        case StatusCode::DeadlineExceeded:
        case StatusCode::NotFound:
        case StatusCode::AlreadyExists:
        case StatusCode::PermissionDenied:
        case StatusCode::ResourceExhausted:
        case StatusCode::FailedPrecondition:
        case StatusCode::Aborted:
        case StatusCode::OutOfRange:
        case StatusCode::Unimplemented:
        case StatusCode::Internal:
        case StatusCode::Unavailable:
        case StatusCode::DataLoss:
        case StatusCode::Unauthenticated:
            return true;
        }
        return false;
    }

    StatusCode code_ { StatusCode::Ok };
    std::string message_;
};

inline std::ostream& operator<<(std::ostream& os, const Status& status)
{
    os << status.toString();
    return os;
}

} // namespace lc
