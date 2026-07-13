#pragma once

#include "foundation/network/i_authorization_provider.hpp"
#include "foundation/time/clock.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace lc {

using AuthorizationHandler = std::function<Status(HttpRequest& request)>;

class NoAuthorizationProvider final : public IAuthorizationProvider {
public:
    [[nodiscard]] Status authorize(HttpRequest& request) override;
};

class BearerTokenAuthorizationProvider final : public IAuthorizationProvider {
public:
    explicit BearerTokenAuthorizationProvider(std::string token);

    [[nodiscard]] Status authorize(HttpRequest& request) override;

private:
    std::string token_;
};

class ApiKeyAuthorizationProvider final : public IAuthorizationProvider {
public:
    ApiKeyAuthorizationProvider(std::string headerName, std::string apiKey);

    [[nodiscard]] Status authorize(HttpRequest& request) override;

private:
    std::string headerName_;
    std::string apiKey_;
};

class BasicAuthorizationProvider final : public IAuthorizationProvider {
public:
    BasicAuthorizationProvider(std::string username, std::string password);

    [[nodiscard]] Status authorize(HttpRequest& request) override;

private:
    std::string username_;
    std::string password_;
};

class OAuthAuthorizationProvider final : public IAuthorizationProvider, public IRefreshableAuthorization {
public:
    static constexpr std::chrono::seconds kDefaultRenewLead { 120 };

    explicit OAuthAuthorizationProvider(
        std::chrono::seconds renewLead = kDefaultRenewLead,
        const WallClock& clock = SystemWallClock::instance()) noexcept;
    explicit OAuthAuthorizationProvider(
        const WallClock& clock,
        std::chrono::seconds renewLead = kDefaultRenewLead) noexcept;

    [[nodiscard]] Status apply(const OAuthCredentials& credentials);
    void onRenew(OAuthTokenRefreshHandler fn) noexcept;
    [[nodiscard]] Status authorize(HttpRequest& request) override;

    void onReady(std::function<void()> fn) noexcept override;
    [[nodiscard]] AuthorizationGate gate() const override;
    void requestRefresh() override;

    [[nodiscard]] std::string refreshToken() const;

private:
    /// Caller must hold `mutex_`. True when the access token is in the proactive-renew window.
    [[nodiscard]] bool shouldPause() const noexcept;
    [[nodiscard]] Status validate(const OAuthCredentials& credentials) const;

    mutable std::mutex mutex_;
    const WallClock* clock_;
    OAuthCredentials credentials_;
    std::chrono::seconds renewLeadResolved_;

    OAuthTokenRefreshHandler renewFn_;
    std::function<void()> readyHook_;
    bool renewPosted_ { false };
};

class FunctionAuthorizationProvider final : public IAuthorizationProvider {
public:
    explicit FunctionAuthorizationProvider(AuthorizationHandler handler);

    [[nodiscard]] Status authorize(HttpRequest& request) override;

private:
    AuthorizationHandler handler_;
};

} // namespace lc
