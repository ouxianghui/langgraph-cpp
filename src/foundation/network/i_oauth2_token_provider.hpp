#pragma once

#include "foundation/network/http_client_types.hpp"

#include <functional>
#include <optional>
#include <string>

namespace lc {

/// Minimal OAuth2 credential surface for `HttpClient`: apply tokens, subscribe to lifecycle hooks,
/// query gate scheduling + bearer access, signal proactive renew (default impl: `OAuth2TokenProvider`).
class IOAuth2TokenProvider {
public:
    virtual ~IOAuth2TokenProvider() = default;

    /// Merge/update stored credentials (same semantics as the former `OAuth2Credentials` → provider flow).
    virtual void apply(const OAuth2Credentials& credentials) = 0;

    /// Called after each successful `apply` (outside provider locks); `HttpClient` wires this to wake workers.
    virtual void onApply(std::function<void()> fn) noexcept = 0;

    /// Optional proactive renew hook when `gate().pause` is true (see `renewNotify`).
    virtual void onRenew(OAuth2TokenRefreshHandler fn) noexcept = 0;

    /// Pause outgoing work + optional absolute wake hint for `wait_until` (single snapshot; lock once).
    [[nodiscard]] virtual OAuth2TokenGate gate() const = 0;

    /// Bearer access token when auto-injecting `Authorization` (empty optional ⇒ omit header).
    [[nodiscard]] virtual std::optional<std::string> accessToken() const = 0;

    /// Fire `onRenew` at most once per defer episode until `apply` clears deferral.
    virtual void renewNotify() = 0;
};

} // namespace lc
