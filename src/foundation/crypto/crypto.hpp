#pragma once

#include "foundation/status/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lc {

enum class HashAlgorithm : std::uint8_t {
    Sha256,
    Sha512,
};

using Bytes = std::vector<std::uint8_t>;

class SecureBytes final {
public:
    SecureBytes() = default;
    explicit SecureBytes(Bytes bytes);
    explicit SecureBytes(std::span<const std::uint8_t> bytes);
    ~SecureBytes();

    SecureBytes(const SecureBytes&) = delete;
    SecureBytes& operator=(const SecureBytes&) = delete;
    SecureBytes(SecureBytes&& other) noexcept;
    SecureBytes& operator=(SecureBytes&& other) noexcept;

    [[nodiscard]] SecureBytes clone() const;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::uint8_t* data() const noexcept;
    [[nodiscard]] std::uint8_t* data() noexcept;
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept;

    void clear() noexcept;

private:
    Bytes bytes_;
};

struct MaskOptions {
    std::size_t visiblePrefix_ { 4 };
    std::size_t visibleSuffix_ { 4 };
    std::size_t minMaskedLength_ { 8 };
    char maskChar_ { '*' };
};

[[nodiscard]] Result<Bytes> random(std::size_t size);
[[nodiscard]] Result<std::string> randomHex(std::size_t byteCount);

[[nodiscard]] Result<Bytes> digest(HashAlgorithm algorithm, std::span<const std::uint8_t> data);
[[nodiscard]] Result<std::string> digestHex(HashAlgorithm algorithm, std::span<const std::uint8_t> data);
[[nodiscard]] Result<std::string> digestHex(HashAlgorithm algorithm, std::string_view data);

[[nodiscard]] Result<Bytes> sign(
    HashAlgorithm algorithm,
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data);
[[nodiscard]] Result<std::string> signHex(
    HashAlgorithm algorithm,
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data);
[[nodiscard]] Result<std::string> signHex(
    HashAlgorithm algorithm,
    std::string_view key,
    std::string_view data);

[[nodiscard]] std::string toHex(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<Bytes> fromHex(std::string_view hex);

[[nodiscard]] std::string maskSecret(
    std::string_view value,
    const MaskOptions& options = {});

[[nodiscard]] bool secureEquals(
    std::span<const std::uint8_t> lhs,
    std::span<const std::uint8_t> rhs) noexcept;

} // namespace lc
