#include "foundation/network/authorization_provider.hpp"

#include "foundation/network/http_client_common.hh"
#include "foundation/utils/http_utils.hpp"

#include <utility>

namespace lgc {
namespace {

[[nodiscard]] Status validateHeaderValue(std::string_view value, std::string_view context)
{
    if (value.empty())
        return Status::invalidArgument(std::string(context) + " cannot be empty");
    if (http_client_detail::containsHeaderInjection(value))
        return Status::invalidArgument(std::string(context) + " contains CR/LF");
    return Status::ok();
}

[[nodiscard]] Status validateHeaderName(std::string_view name)
{
    if (!http_client_detail::isValidHeaderName(name))
        return Status::invalidArgument("authorization header name is invalid");
    return Status::ok();
}

[[nodiscard]] bool hasAuthorizationHeader(const HttpRequest& request)
{
    return request.header("authorization").has_value();
}

} // namespace

Status NoAuthorizationProvider::authorize(HttpRequest&)
{
    return Status::ok();
}

BearerTokenAuthorizationProvider::BearerTokenAuthorizationProvider(std::string token)
    : token_(std::move(token))
{
}

Status BearerTokenAuthorizationProvider::authorize(HttpRequest& request)
{
    if (hasAuthorizationHeader(request))
        return Status::ok();
    if (auto status = validateHeaderValue(token_, "bearer token"); !status.isOk())
        return status;
    request.setHeader("Authorization", "Bearer " + token_);
    return Status::ok();
}

ApiKeyAuthorizationProvider::ApiKeyAuthorizationProvider(std::string headerName, std::string apiKey)
    : headerName_(std::move(headerName))
    , apiKey_(std::move(apiKey))
{
}

Status ApiKeyAuthorizationProvider::authorize(HttpRequest& request)
{
    if (auto status = validateHeaderName(headerName_); !status.isOk())
        return status;
    if (request.header(headerName_).has_value())
        return Status::ok();
    if (auto status = validateHeaderValue(apiKey_, "API key"); !status.isOk())
        return status;
    request.setHeader(headerName_, apiKey_);
    return Status::ok();
}

BasicAuthorizationProvider::BasicAuthorizationProvider(std::string username, std::string password)
    : username_(std::move(username))
    , password_(std::move(password))
{
}

Status BasicAuthorizationProvider::authorize(HttpRequest& request)
{
    if (hasAuthorizationHeader(request))
        return Status::ok();
    if (username_.empty())
        return Status::invalidArgument("basic auth username cannot be empty");
    if (http_client_detail::containsHeaderInjection(username_)
        || http_client_detail::containsHeaderInjection(password_)) {
        return Status::invalidArgument("basic auth credentials contain CR/LF");
    }
    request.setHeader("Authorization", basicAuthHeaderValue(username_, password_));
    return Status::ok();
}

OAuthAuthorizationProvider::OAuthAuthorizationProvider(std::chrono::seconds renewLead, const WallClock& clock) noexcept
    : clock_(&clock)
    , renewLeadResolved_(renewLead < std::chrono::seconds::zero() ? std::chrono::seconds::zero() : renewLead)
{
}

OAuthAuthorizationProvider::OAuthAuthorizationProvider(const WallClock& clock, std::chrono::seconds renewLead) noexcept
    : OAuthAuthorizationProvider(renewLead, clock)
{
}

Status OAuthAuthorizationProvider::validate(const OAuthCredentials& credentials) const
{
    if (credentials.tokenRenewLeadTime_.has_value() && *credentials.tokenRenewLeadTime_ < std::chrono::seconds::zero())
        return Status::invalidArgument("OAuth token renew lead cannot be negative");
    if (credentials.accessToken_.empty() && credentials.accessTokenExpiresAt_.has_value())
        return Status::invalidArgument("OAuth access token expiry requires an access token");
    return Status::ok();
}

Status OAuthAuthorizationProvider::apply(const OAuthCredentials& credentials)
{
    if (auto status = validate(credentials); !status.isOk())
        return status;

    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        credentials_.accessToken_ = credentials.accessToken_;
        credentials_.refreshToken_ = credentials.refreshToken_;
        credentials_.accessTokenExpiresAt_ = credentials.accessTokenExpiresAt_;
        credentials_.tokenRenewLeadTime_.reset();
        if (credentials.tokenRenewLeadTime_.has_value()) {
            renewLeadResolved_ = *credentials.tokenRenewLeadTime_ < std::chrono::seconds::zero()
                ? std::chrono::seconds::zero()
                : *credentials.tokenRenewLeadTime_;
        }
        if (!shouldPause())
            renewPosted_ = false;
        hook = readyHook_;
    }
    if (hook) {
        try {
            hook();
        } catch (...) {
            return Status::unknown("OAuth ready callback failed");
        }
    }
    return Status::ok();
}

void OAuthAuthorizationProvider::onRenew(OAuthTokenRefreshHandler fn) noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    renewFn_ = std::move(fn);
}

Status OAuthAuthorizationProvider::authorize(HttpRequest& request)
{
    if (hasAuthorizationHeader(request))
        return Status::ok();

    std::optional<std::string> token;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!credentials_.accessToken_.empty())
            token = credentials_.accessToken_;
    }
    if (!token)
        return Status::ok();
    if (auto status = validateHeaderValue(*token, "OAuth access token"); !status.isOk())
        return status;
    request.setHeader("Authorization", "Bearer " + *token);
    return Status::ok();
}

void OAuthAuthorizationProvider::onReady(std::function<void()> fn) noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    readyHook_ = std::move(fn);
}

bool OAuthAuthorizationProvider::shouldPause() const noexcept
{
    if (!credentials_.accessTokenExpiresAt_.has_value() || credentials_.accessToken_.empty())
        return false;
    const auto now = clock_->now();
    return now >= (*credentials_.accessTokenExpiresAt_ - renewLeadResolved_);
}

AuthorizationGate OAuthAuthorizationProvider::gate() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    AuthorizationGate out;
    out.paused_ = shouldPause();
    if (credentials_.accessTokenExpiresAt_.has_value() && !credentials_.accessToken_.empty())
        out.resumeAt_ = *credentials_.accessTokenExpiresAt_ - renewLeadResolved_;
    return out;
}

void OAuthAuthorizationProvider::requestRefresh()
{
    OAuthTokenRefreshHandler renew;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!shouldPause())
            return;
        if (renewPosted_)
            return;
        renewPosted_ = true;
        renew = renewFn_;
    }
    if (renew) {
        try {
            renew(OAuthTokenRefreshReason::ProactiveRenewWindow);
        } catch (...) {
        }
    }
}

std::string OAuthAuthorizationProvider::refreshToken() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return credentials_.refreshToken_;
}

FunctionAuthorizationProvider::FunctionAuthorizationProvider(AuthorizationHandler handler)
    : handler_(std::move(handler))
{
}

Status FunctionAuthorizationProvider::authorize(HttpRequest& request)
{
    if (!handler_)
        return Status::invalidArgument("authorization handler is required");
    return handler_(request);
}

} // namespace lgc
