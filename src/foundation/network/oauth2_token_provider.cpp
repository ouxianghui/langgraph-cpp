#include "foundation/network/oauth2_token_provider.hpp"

namespace lc {

OAuth2TokenProvider::OAuth2TokenProvider(std::chrono::seconds renewLead) noexcept
    : renewLeadResolved_(renewLead)
{
}

void OAuth2TokenProvider::apply(const OAuth2Credentials& credentials)
{
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        credentials_.accessToken_ = credentials.accessToken_;
        credentials_.refreshToken_ = credentials.refreshToken_;
        credentials_.accessTokenExpiresAt_ = credentials.accessTokenExpiresAt_;
        credentials_.tokenRenewLeadTime_.reset();
        if (credentials.tokenRenewLeadTime_.has_value())
            renewLeadResolved_ = *credentials.tokenRenewLeadTime_;
        if (!shouldPause())
            renewPosted_ = false;
        hook = applyHook_;
    }
    if (hook)
        hook();
}

void OAuth2TokenProvider::onApply(std::function<void()> fn) noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    applyHook_ = std::move(fn);
}

void OAuth2TokenProvider::onRenew(OAuth2TokenRefreshHandler fn) noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    renewFn_ = std::move(fn);
}

bool OAuth2TokenProvider::shouldPause() const noexcept
{
    if (!credentials_.accessTokenExpiresAt_.has_value() || credentials_.accessToken_.empty())
        return false;
    const auto now = std::chrono::system_clock::now();
    return now >= (*credentials_.accessTokenExpiresAt_ - renewLeadResolved_);
}

OAuth2TokenGate OAuth2TokenProvider::gate() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    OAuth2TokenGate out;
    out.pause = shouldPause();
    if (credentials_.accessTokenExpiresAt_.has_value() && !credentials_.accessToken_.empty())
        out.wakeAt = *credentials_.accessTokenExpiresAt_ - renewLeadResolved_;
    return out;
}

std::optional<std::string> OAuth2TokenProvider::accessToken() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (credentials_.accessToken_.empty())
        return std::nullopt;
    return credentials_.accessToken_;
}

std::string OAuth2TokenProvider::refreshToken() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return credentials_.refreshToken_;
}

void OAuth2TokenProvider::renewNotify()
{
    OAuth2TokenRefreshHandler renew;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!shouldPause())
            return;
        if (renewPosted_)
            return;
        renewPosted_ = true;
        renew = renewFn_;
    }
    if (renew)
        renew(OAuth2TokenRefreshReason::ProactiveRenewWindow);
}

} // namespace lc
