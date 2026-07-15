#pragma once

#include "foundation/serialization/content_envelope.hpp"

#include <span>
#include <string>
#include <string_view>

namespace lgc::content_envelope_detail {

inline constexpr std::string_view kEnvelopeContentType = "application/vnd.langgraph-cpp.content-envelope+json";
inline constexpr std::string_view kUtf8Encoding = "utf-8";
inline constexpr std::string_view kChecksumSha256 = "sha256";
inline constexpr std::string_view kAesGcmName = "aes-gcm";

[[nodiscard]] std::span<const std::uint8_t> bytesView(const Bytes& bytes) noexcept;
[[nodiscard]] std::span<const std::byte> byteSpan(const Bytes& bytes) noexcept;
[[nodiscard]] Bytes bytesFromString(std::string_view text);
[[nodiscard]] std::string stringFromBytes(const Bytes& bytes);
[[nodiscard]] std::string compressionNameJson(CompressionAlgorithm algorithm);

} // namespace lgc::content_envelope_detail
