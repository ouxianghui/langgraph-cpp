#pragma once

#include "foundation/crypto/crypto.hpp"
#include "foundation/status/result.hpp"

#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lc {

struct Secret final {
    Secret() = default;
    explicit Secret(std::string value);
    explicit Secret(SecureBytes value);

    Secret(const Secret& other);
    Secret& operator=(const Secret& other);
    Secret(Secret&& other) noexcept = default;
    Secret& operator=(Secret&& other) noexcept = default;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] std::string stringValue() const;
    [[nodiscard]] std::string masked() const;

    void clear() noexcept;

private:
    SecureBytes value_;
};

class ISecrets {
public:
    virtual ~ISecrets() = default;

    [[nodiscard]] virtual Result<Secret> get(std::string_view name) const = 0;
};

class EnvSecrets final : public ISecrets {
public:
    [[nodiscard]] Result<Secret> get(std::string_view name) const override;
};

class MemorySecrets final : public ISecrets {
public:
    MemorySecrets() = default;
    explicit MemorySecrets(std::unordered_map<std::string, Secret> secrets);

    [[nodiscard]] Result<Secret> get(std::string_view name) const override;

    [[nodiscard]] Status set(std::string name, Secret secret);
    [[nodiscard]] Status set(std::string name, std::string value);
    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] Status remove(std::string_view name);
    void clear() noexcept;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Secret> secrets_;
};

[[nodiscard]] Status validateSecretKey(std::string_view name);

} // namespace lc
