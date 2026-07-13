#include "foundation/config/env.hpp"

#include <cstdlib>

namespace lc {

std::optional<std::string> Env::get(std::string_view name)
{
    if (!validateEnvName(name).isOk())
        return std::nullopt;

    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr)
        return std::nullopt;
    return std::string(value);
}

std::string Env::valueOr(std::string_view name, std::string fallback)
{
    auto value = get(name);
    if (!value.has_value())
        return fallback;
    return *value;
}

Result<std::string> Env::require(std::string_view name)
{
    if (auto status = validateEnvName(name); !status.isOk())
        return status;

    auto value = get(name);
    if (!value.has_value()) {
        std::string message("missing environment variable: ");
        message.append(name);
        return Status::notFound(std::move(message));
    }
    return *value;
}

Status validateEnvName(std::string_view name)
{
    if (name.empty())
        return Status::invalidArgument("environment variable name cannot be empty");
    if (name.find('=') != std::string_view::npos)
        return Status::invalidArgument("environment variable name cannot contain '='");
    return Status::ok();
}

} // namespace lc
