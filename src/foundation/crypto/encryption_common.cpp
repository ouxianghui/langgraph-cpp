#include "foundation/crypto/encryption_common.hh"

#include <utility>

namespace lc::encryption_detail {

[[nodiscard]] std::span<const std::uint8_t> bytesFromString(std::string_view text) noexcept
{
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size());
}

[[nodiscard]] std::string stringFromBytes(std::span<const std::uint8_t> bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] Bytes byteVectorFromString(std::string_view text)
{
    const auto bytes = bytesFromString(text);
    return Bytes(bytes.begin(), bytes.end());
}
[[nodiscard]] Status validateKeyId(std::string_view keyId, const EncryptedPayloadOptions& options)
{
    if (keyId.size() > options.maxKeyIdBytes_)
        return Status::resourceExhausted("encrypted payload key_id is too large");
    return Status::ok();
}

[[nodiscard]] Status validateCiphertextSize(std::size_t size, const EncryptedPayloadOptions& options)
{
    if (size > options.maxCiphertextBytes_)
        return Status::resourceExhausted("encrypted payload ciphertext is too large");
    return Status::ok();
}

[[nodiscard]] Status validateAssociatedDataSize(std::size_t size, const EncryptedPayloadOptions& options)
{
    if (size > options.maxAssociatedDataBytes_)
        return Status::resourceExhausted("encrypted payload associated data is too large");
    return Status::ok();
}

} // namespace lc::encryption_detail
