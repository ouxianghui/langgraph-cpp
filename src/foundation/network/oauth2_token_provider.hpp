#pragma once

#include "foundation/network/i_oauth2_token_provider.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace lc {

/// Default `IOAuth2TokenProvider`: `OAuth2Credentials` snapshot + resolved renew-lead duration.
class OAuth2TokenProvider final : public IOAuth2TokenProvider {
public:
    /// Initial renew-lead when no `OAuth2Credentials::tokenRenewLeadTime_` has been applied yet.
    static constexpr std::chrono::seconds kDefaultRenewLead { 120 };

    explicit OAuth2TokenProvider(std::chrono::seconds renewLead = kDefaultRenewLead) noexcept;

    void apply(const OAuth2Credentials& credentials) override;

    void onApply(std::function<void()> fn) noexcept override;

    void onRenew(OAuth2TokenRefreshHandler fn) noexcept override;

    [[nodiscard]] OAuth2TokenGate gate() const override;

    [[nodiscard]] std::optional<std::string> accessToken() const override;

    void renewNotify() override;

    /// Convenience when holding `shared_ptr<OAuth2TokenProvider>` (not part of `IOAuth2TokenProvider`).
    [[nodiscard]] std::string refreshToken() const;

private:
    /// Caller must hold `mutex_`. True when the access token is in the proactive-renew window.
    [[nodiscard]] bool shouldPause() const noexcept;

    mutable std::mutex mutex_;
    OAuth2Credentials credentials_;
    std::chrono::seconds renewLeadResolved_;

    OAuth2TokenRefreshHandler renewFn_;
    std::function<void()> applyHook_;
    bool renewPosted_ { false };
};

} // namespace lc
