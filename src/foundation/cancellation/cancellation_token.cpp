#include "foundation/cancellation/cancellation_token.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace lc {

#if LC_HAS_STD_STOP_TOKEN
struct CancellationState final {
    std::stop_source source;
    mutable std::mutex mutex;
    std::string reason;
    bool cancelStarted { false };
};

namespace detail {

struct CancellationCallbackRegistration final {
    using Callback = std::stop_callback<CancellationToken::Callback>;

    CancellationCallbackRegistration(
        std::weak_ptr<CancellationState> state,
        std::stop_token token,
        CancellationToken::Callback callback)
        : state_(std::move(state))
        , callback_(std::move(token), std::move(callback))
    {
    }

    std::weak_ptr<CancellationState> state_;
    Callback callback_;
};

} // namespace detail
#else
struct CancellationState final {
    std::atomic<bool> requested { false };
    mutable std::mutex mutex;
    std::string reason;
    bool cancelStarted { false };
    std::uint64_t nextCallbackId { 1 };
    std::vector<std::pair<std::uint64_t, CancellationToken::Callback>> callbacks;
};
#endif

namespace {

[[nodiscard]] std::string effectiveMessage(const std::string& primary, const std::string& fallback)
{
    return primary.empty() ? fallback : primary;
}

void throwIfNotOk(Status status)
{
    if (!status.isOk())
        throw OperationInterrupted(std::move(status));
}

} // namespace

CancellationRegistration::CancellationRegistration() noexcept = default;

#if LC_HAS_STD_STOP_TOKEN
CancellationRegistration::CancellationRegistration(
    std::unique_ptr<detail::CancellationCallbackRegistration> registration) noexcept
    : registration_(std::move(registration))
{
}

CancellationRegistration::~CancellationRegistration() = default;

CancellationRegistration::CancellationRegistration(CancellationRegistration&& other) noexcept = default;

CancellationRegistration& CancellationRegistration::operator=(CancellationRegistration&& other) noexcept = default;

void CancellationRegistration::unregister() noexcept
{
    registration_.reset();
}

bool CancellationRegistration::registered() const noexcept
{
    if (!registration_)
        return false;

    auto state = registration_->state_.lock();
    if (!state)
        return false;

    return state->source.stop_possible() && !state->source.stop_requested();
}
#else
CancellationRegistration::CancellationRegistration(std::weak_ptr<CancellationState> state, std::uint64_t id) noexcept
    : state_(std::move(state))
    , id_(id)
{
}

CancellationRegistration::~CancellationRegistration()
{
    unregister();
}

CancellationRegistration::CancellationRegistration(CancellationRegistration&& other) noexcept
    : state_(std::move(other.state_))
    , id_(std::exchange(other.id_, 0))
{
}

CancellationRegistration& CancellationRegistration::operator=(CancellationRegistration&& other) noexcept
{
    if (this == &other)
        return *this;

    unregister();
    state_ = std::move(other.state_);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

void CancellationRegistration::unregister() noexcept
{
    if (id_ == 0)
        return;

    auto state = state_.lock();
    if (!state) {
        id_ = 0;
        return;
    }

    std::lock_guard lock(state->mutex);
    const auto id = id_;
    auto& callbacks = state->callbacks;
    callbacks.erase(
        std::remove_if(
            callbacks.begin(),
            callbacks.end(),
            [id](const auto& item) {
                return item.first == id;
            }),
        callbacks.end());
    id_ = 0;
}

bool CancellationRegistration::registered() const noexcept
{
    if (id_ == 0)
        return false;
    auto state = state_.lock();
    if (!state)
        return false;

    std::lock_guard lock(state->mutex);
    if (state->cancelStarted)
        return false;

    const auto id = id_;
    return std::any_of(
        state->callbacks.begin(),
        state->callbacks.end(),
        [id](const auto& item) {
            return item.first == id;
        });
}
#endif

OperationInterrupted::OperationInterrupted(Status status)
    : std::runtime_error(status.toString())
    , status_(std::move(status))
{
}

const Status& OperationInterrupted::status() const noexcept
{
    return status_;
}

CancellationToken CancellationToken::none() noexcept
{
    return CancellationToken();
}

CancellationToken::CancellationToken(std::shared_ptr<CancellationState> state) noexcept
    : state_(std::move(state))
{
}

bool CancellationToken::cancellable() const noexcept
{
#if LC_HAS_STD_STOP_TOKEN
    return state_ && state_->source.get_token().stop_possible();
#else
    return static_cast<bool>(state_);
#endif
}

bool CancellationToken::cancelled() const noexcept
{
#if LC_HAS_STD_STOP_TOKEN
    return state_ && state_->source.stop_requested();
#else
    return state_ && state_->requested.load(std::memory_order_acquire);
#endif
}

std::string CancellationToken::reason() const
{
    if (!state_)
        return {};

    std::lock_guard lock(state_->mutex);
    return state_->reason;
}

#if LC_HAS_STD_STOP_TOKEN
std::stop_token CancellationToken::nativeToken() const noexcept
{
    if (!state_)
        return {};
    return state_->source.get_token();
}
#endif

Status CancellationToken::check(std::string message) const
{
    if (!cancelled())
        return Status::ok();

    return Status::cancelled(effectiveMessage(reason(), message));
}

CancellationRegistration CancellationToken::onCancel(Callback callback) const
{
    if (!state_ || !callback)
        return {};

#if LC_HAS_STD_STOP_TOKEN
    const auto token = state_->source.get_token();
    if (token.stop_requested()) {
        try {
            callback();
        } catch (...) {
        }
        return {};
    }

    auto registration = std::make_unique<detail::CancellationCallbackRegistration>(
        state_,
        token,
        std::move(callback));

    if (token.stop_requested())
        return {};

    return CancellationRegistration(std::move(registration));
#else
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->cancelStarted) {
            const auto id = state_->nextCallbackId++;
            state_->callbacks.emplace_back(id, std::move(callback));
            return CancellationRegistration(state_, id);
        }
    }

    try {
        callback();
    } catch (...) {
    }
    return {};
#endif
}

Status CancellationToken::check(
    const Clock& clock,
    const Deadline& deadline,
    std::string cancelMessage,
    std::string deadlineMessage) const
{
    auto status = check(std::move(cancelMessage));
    if (!status.isOk())
        return status;

    return deadline.statusIfExpired(clock, std::move(deadlineMessage));
}

void CancellationToken::throwIfCancelled(std::string message) const
{
    throwIfNotOk(check(std::move(message)));
}

void CancellationToken::throwIfCancelledOrDeadlineExceeded(
    const Clock& clock,
    const Deadline& deadline,
    std::string cancelMessage,
    std::string deadlineMessage) const
{
    throwIfNotOk(check(
        clock,
        deadline,
        std::move(cancelMessage),
        std::move(deadlineMessage)));
}

CancellationSource::CancellationSource()
    : state_(std::make_shared<CancellationState>())
{
}

CancellationToken CancellationSource::token() const noexcept
{
    return CancellationToken(state_);
}

bool CancellationSource::cancelled() const noexcept
{
#if LC_HAS_STD_STOP_TOKEN
    return state_->source.stop_requested();
#else
    return state_->requested.load(std::memory_order_acquire);
#endif
}

std::string CancellationSource::reason() const
{
    std::lock_guard lock(state_->mutex);
    return state_->reason;
}

bool CancellationSource::cancel(std::string reason)
{
#if LC_HAS_STD_STOP_TOKEN
    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelStarted)
            return false;
        state_->cancelStarted = true;
        state_->reason = std::move(reason);
    }

    return state_->source.request_stop();
#else
    std::vector<CancellationToken::Callback> callbacks;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelStarted)
            return false;

        state_->cancelStarted = true;
        state_->reason = std::move(reason);
        state_->requested.store(true, std::memory_order_release);
        callbacks.reserve(state_->callbacks.size());
        for (auto& callback : state_->callbacks)
            callbacks.push_back(std::move(callback.second));
        state_->callbacks.clear();
    }

    for (auto& callback : callbacks) {
        try {
            callback();
        } catch (...) {
        }
    }
    return true;
#endif
}

Status checkCancelled(const CancellationToken& token, std::string message)
{
    return token.check(std::move(message));
}

Status checkCancelled(
    const CancellationToken& token,
    const Clock& clock,
    const Deadline& deadline,
    std::string cancelMessage,
    std::string deadlineMessage)
{
    return token.check(
        clock,
        deadline,
        std::move(cancelMessage),
        std::move(deadlineMessage));
}

void throwIfCancelled(const CancellationToken& token, std::string message)
{
    token.throwIfCancelled(std::move(message));
}

void throwIfCancelledOrDeadlineExceeded(
    const CancellationToken& token,
    const Clock& clock,
    const Deadline& deadline,
    std::string cancelMessage,
    std::string deadlineMessage)
{
    token.throwIfCancelledOrDeadlineExceeded(
        clock,
        deadline,
        std::move(cancelMessage),
        std::move(deadlineMessage));
}

} // namespace lc
