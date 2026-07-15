#pragma once

#include "foundation/network/http_client_types.hpp"

#include <functional>

namespace lgc {

/// Request-auth extension point for outbound HTTP calls.
class IAuthorizationProvider {
public:
    virtual ~IAuthorizationProvider() = default;

    [[nodiscard]] virtual Status authorize(HttpRequest& request) = 0;
};

/// Optional readiness/refresh contract for auth providers with expiring credentials.
class IRefreshableAuthorization {
public:
    virtual ~IRefreshableAuthorization() = default;

    /// Called when a previously gated provider becomes ready again. Callbacks may run synchronously
    /// from provider-owned calls; implementations must not invoke them while holding internal locks.
    virtual void onReady(std::function<void()> fn) noexcept = 0;

    [[nodiscard]] virtual AuthorizationGate gate() const = 0;

    /// Signals that `gate().paused_` blocked work and the provider should refresh or renew.
    virtual void requestRefresh() = 0;
};

} // namespace lgc
