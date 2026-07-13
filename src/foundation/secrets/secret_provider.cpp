#include "foundation/secrets/secret_provider.hpp"

#include "foundation/config/env.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

namespace lc {
namespace {

constexpr std::size_t kMaxSecretKeyBytes = 256;

[[nodiscard]] bool validSecretKeyChar(unsigned char ch) noexcept
{
    return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == '/';
}

} // namespace

Secret::Secret(std::string value)
    : value_(SecureBytes(std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(value.data()),
          value.size())))
{
    std::ranges::fill(value, '\0');
}

Secret::Secret(SecureBytes value)
    : value_(std::move(value))
{
}

Secret::Secret(const Secret& other)
    : value_(other.value_.clone())
{
}

Secret& Secret::operator=(const Secret& other)
{
    if (this == &other)
        return *this;
    value_ = other.value_.clone();
    return *this;
}

bool Secret::empty() const noexcept
{
    return value_.empty();
}

std::size_t Secret::size() const noexcept
{
    return value_.size();
}

std::span<const std::uint8_t> Secret::bytes() const noexcept
{
    return value_.span();
}

std::string Secret::stringValue() const
{
    const auto bytes = value_.span();
    if (bytes.empty())
        return {};
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
}

std::string Secret::masked() const
{
    return maskSecret(stringValue());
}

void Secret::clear() noexcept
{
    value_.clear();
}

Result<Secret> EnvSecrets::get(std::string_view name) const
{
    if (auto status = validateSecretKey(name); !status.isOk())
        return errorResult<Secret>(std::move(status));

    auto value = Env::get(name);
    if (!value)
        return errorResult<Secret>(Status::notFound("secret not found: " + std::string(name)));
    return okResult(Secret(std::move(*value)));
}

MemorySecrets::MemorySecrets(std::unordered_map<std::string, Secret> secrets)
    : secrets_(std::move(secrets))
{
}

Result<Secret> MemorySecrets::get(std::string_view name) const
{
    if (auto status = validateSecretKey(name); !status.isOk())
        return errorResult<Secret>(std::move(status));

    std::lock_guard lock(mutex_);
    const auto it = secrets_.find(std::string(name));
    if (it == secrets_.end())
        return errorResult<Secret>(Status::notFound("secret not found: " + std::string(name)));
    return okResult(it->second);
}

Status MemorySecrets::set(std::string name, Secret secret)
{
    if (auto status = validateSecretKey(name); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    auto it = secrets_.find(name);
    if (it != secrets_.end())
        it->second.clear();
    secrets_[std::move(name)] = std::move(secret);
    return Status::ok();
}

Status MemorySecrets::set(std::string name, std::string value)
{
    return set(std::move(name), Secret(std::move(value)));
}

bool MemorySecrets::contains(std::string_view name) const
{
    if (!validateSecretKey(name).isOk())
        return false;

    std::lock_guard lock(mutex_);
    return secrets_.contains(std::string(name));
}

Status MemorySecrets::remove(std::string_view name)
{
    if (auto status = validateSecretKey(name); !status.isOk())
        return status;

    std::lock_guard lock(mutex_);
    const auto it = secrets_.find(std::string(name));
    if (it == secrets_.end())
        return Status::notFound("secret not found: " + std::string(name));
    it->second.clear();
    secrets_.erase(it);
    return Status::ok();
}

void MemorySecrets::clear() noexcept
{
    try {
        std::lock_guard lock(mutex_);
        for (auto& [_, secret] : secrets_)
            secret.clear();
        secrets_.clear();
    } catch (...) {
    }
}

Status validateSecretKey(std::string_view name)
{
    if (name.empty())
        return Status::invalidArgument("secret key cannot be empty");
    if (name.size() > kMaxSecretKeyBytes)
        return Status::invalidArgument("secret key is too long");

    for (const auto ch : name) {
        if (!validSecretKeyChar(static_cast<unsigned char>(ch)))
            return Status::invalidArgument("secret key contains invalid characters");
    }
    return Status::ok();
}

} // namespace lc
