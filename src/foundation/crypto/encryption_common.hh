#pragma once

#include "foundation/crypto/encryption.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lc::encryption_detail {

inline constexpr std::size_t kAesGcmNonceSize = 12;
inline constexpr std::size_t kAesGcmTagSize = 16;
inline constexpr std::uint32_t kEncryptedPayloadVersion = 1;
inline constexpr std::string_view kEncryptedCheckpointContentType =
    "application/vnd.langgraph-cpp.encrypted-checkpoint+json";

[[nodiscard]] std::span<const std::uint8_t> bytesFromString(std::string_view text) noexcept;
[[nodiscard]] std::string stringFromBytes(std::span<const std::uint8_t> bytes);
[[nodiscard]] Bytes byteVectorFromString(std::string_view text);
[[nodiscard]] Status validateKeyId(std::string_view keyId, const EncryptedPayloadOptions& options);
[[nodiscard]] Status validateCiphertextSize(std::size_t size, const EncryptedPayloadOptions& options);
[[nodiscard]] Status validateAssociatedDataSize(std::size_t size, const EncryptedPayloadOptions& options);

} // namespace lc::encryption_detail
