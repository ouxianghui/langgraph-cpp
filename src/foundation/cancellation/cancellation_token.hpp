#pragma once

#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"
#include "foundation/time/deadline.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <version>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
#include <stop_token>
#define LC_HAS_STD_STOP_TOKEN 1
#else
#define LC_HAS_STD_STOP_TOKEN 0
#endif

#include <string>

namespace lc {

struct CancellationState;

namespace detail {
struct CancellationCallbackRegistration;
} // namespace detail

class CancellationRegistration final {
public:
    CancellationRegistration() noexcept;
    ~CancellationRegistration();

    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;
    CancellationRegistration(CancellationRegistration&& other) noexcept;
    CancellationRegistration& operator=(CancellationRegistration&& other) noexcept;

    void unregister() noexcept;
    [[nodiscard]] bool registered() const noexcept;

private:
    friend class CancellationToken;

#if LC_HAS_STD_STOP_TOKEN
    explicit CancellationRegistration(
        std::unique_ptr<detail::CancellationCallbackRegistration> registration) noexcept;

    std::unique_ptr<detail::CancellationCallbackRegistration> registration_;
#else
    CancellationRegistration(std::weak_ptr<CancellationState> state, std::uint64_t id) noexcept;

    std::weak_ptr<CancellationState> state_;
    std::uint64_t id_ { 0 };
#endif
};

class OperationInterrupted final : public std::runtime_error {
public:
    explicit OperationInterrupted(Status status);

    [[nodiscard]] const Status& status() const noexcept;

private:
    Status status_;
};

/// Immutable handle passed through graph/runtime operations.
///
/// A default-constructed token is not cancellable. Tokens returned by
/// `CancellationSource::token()` are cheap to copy and remain valid even after the source is moved
/// or destroyed.
class CancellationToken final {
public:
    using Callback = std::function<void()>;

    CancellationToken() noexcept = default;

    [[nodiscard]] static CancellationToken none() noexcept;

    [[nodiscard]] bool cancellable() const noexcept;
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] std::string reason() const;
#if LC_HAS_STD_STOP_TOKEN
    [[nodiscard]] std::stop_token nativeToken() const noexcept;
#endif

    [[nodiscard]] Status check(
        std::string message = "operation cancelled") const;

    /// Register a callback invoked once when cancellation is requested.
    ///
    /// If the token is already cancelled, the callback is invoked synchronously before returning and
    /// the returned registration is empty. Callbacks should be short and must not throw.
    [[nodiscard]] CancellationRegistration onCancel(Callback callback) const;

    [[nodiscard]] Status check(
        const Clock& clock,
        const Deadline& deadline,
        std::string cancelMessage = "operation cancelled",
        std::string deadlineMessage = "deadline exceeded") const;

    void throwIfCancelled(std::string message = "operation cancelled") const;

    void throwIfCancelledOrDeadlineExceeded(
        const Clock& clock,
        const Deadline& deadline,
        std::string cancelMessage = "operation cancelled",
        std::string deadlineMessage = "deadline exceeded") const;

private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<CancellationState> state) noexcept;

    std::shared_ptr<CancellationState> state_;
};

/// Mutable owner used by callers to request cancellation.
///
/// The first cancellation request wins and records the reason. Subsequent requests are ignored and
/// return false.
class CancellationSource final {
public:
    CancellationSource();

    [[nodiscard]] CancellationToken token() const noexcept;
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] std::string reason() const;

    bool cancel(std::string reason = "operation cancelled");

private:
    std::shared_ptr<CancellationState> state_;
};

[[nodiscard]] Status checkCancelled(
    const CancellationToken& token,
    std::string message = "operation cancelled");

[[nodiscard]] Status checkCancelled(
    const CancellationToken& token,
    const Clock& clock,
    const Deadline& deadline,
    std::string cancelMessage = "operation cancelled",
    std::string deadlineMessage = "deadline exceeded");

void throwIfCancelled(
    const CancellationToken& token,
    std::string message = "operation cancelled");

void throwIfCancelledOrDeadlineExceeded(
    const CancellationToken& token,
    const Clock& clock,
    const Deadline& deadline,
    std::string cancelMessage = "operation cancelled",
    std::string deadlineMessage = "deadline exceeded");

} // namespace lc
