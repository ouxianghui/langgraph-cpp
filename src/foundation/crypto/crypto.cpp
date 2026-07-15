#include "foundation/crypto/crypto.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace lgc {
namespace {

constexpr char kHexAlphabet[] = "0123456789abcdef";

[[nodiscard]] const EVP_MD* digestFor(HashAlgorithm algorithm) noexcept
{
    switch (algorithm) {
    case HashAlgorithm::Sha256:
        return EVP_sha256();
    case HashAlgorithm::Sha512:
        return EVP_sha512();
    }
    return nullptr;
}

[[nodiscard]] Result<std::span<const std::uint8_t>> bytesFromString(std::string_view text) noexcept
{
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size());
}

[[nodiscard]] std::uint8_t hexValue(char value) noexcept
{
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
        return static_cast<std::uint8_t>(value - 'A' + 10);
    return 0xff;
}

class EvpDigestContext final {
public:
    EvpDigestContext() noexcept
        : context_(EVP_MD_CTX_new())
    {
    }

    EvpDigestContext(const EvpDigestContext&) = delete;
    EvpDigestContext& operator=(const EvpDigestContext&) = delete;

    ~EvpDigestContext()
    {
        EVP_MD_CTX_free(context_);
    }

    [[nodiscard]] EVP_MD_CTX* get() const noexcept { return context_; }

private:
    EVP_MD_CTX* context_ { nullptr };
};

} // namespace

SecureBytes::SecureBytes(Bytes bytes)
    : bytes_(std::move(bytes))
{
}

SecureBytes::SecureBytes(std::span<const std::uint8_t> bytes)
    : bytes_(bytes.begin(), bytes.end())
{
}

SecureBytes::~SecureBytes()
{
    clear();
}

SecureBytes::SecureBytes(SecureBytes&& other) noexcept
    : bytes_(std::move(other.bytes_))
{
}

SecureBytes& SecureBytes::operator=(SecureBytes&& other) noexcept
{
    if (this == &other)
        return *this;
    clear();
    bytes_ = std::move(other.bytes_);
    return *this;
}

SecureBytes SecureBytes::clone() const
{
    return SecureBytes(span());
}

bool SecureBytes::empty() const noexcept
{
    return bytes_.empty();
}

std::size_t SecureBytes::size() const noexcept
{
    return bytes_.size();
}

const std::uint8_t* SecureBytes::data() const noexcept
{
    return bytes_.data();
}

std::uint8_t* SecureBytes::data() noexcept
{
    return bytes_.data();
}

std::span<const std::uint8_t> SecureBytes::span() const noexcept
{
    return std::span<const std::uint8_t>(bytes_.data(), bytes_.size());
}

void SecureBytes::clear() noexcept
{
    if (!bytes_.empty())
        OPENSSL_cleanse(bytes_.data(), bytes_.size());
    bytes_.clear();
}

Result<Bytes> random(std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Status::resourceExhausted("random byte request is too large");

    Bytes bytes(size);
    if (size == 0)
        return bytes;

    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return Status::unavailable("failed to generate secure random bytes");

    return bytes;
}

Result<std::string> randomHex(std::size_t byteCount)
{
    auto bytes = random(byteCount);
    if (!bytes.isOk())
        return bytes.status();

    return toHex(*bytes);
}

Result<Bytes> digest(HashAlgorithm algorithm, std::span<const std::uint8_t> data)
{
    const auto* digest = digestFor(algorithm);
    if (digest == nullptr)
        return Status::invalidArgument("unsupported hash algorithm");

    EvpDigestContext context;
    if (context.get() == nullptr)
        return Status::resourceExhausted("failed to allocate hash context");

    if (EVP_DigestInit_ex(context.get(), digest, nullptr) != 1)
        return Status::internal("failed to initialize hash context");
    if (!data.empty() && EVP_DigestUpdate(context.get(), data.data(), data.size()) != 1)
        return Status::internal("failed to update hash context");

    Bytes out(static_cast<std::size_t>(EVP_MD_get_size(digest)));
    unsigned int outSize = 0;
    if (EVP_DigestFinal_ex(context.get(), out.data(), &outSize) != 1)
        return Status::internal("failed to finalize hash");

    out.resize(outSize);
    return out;
}

Result<std::string> digestHex(HashAlgorithm algorithm, std::span<const std::uint8_t> data)
{
    auto bytes = digest(algorithm, data);
    if (!bytes.isOk())
        return bytes.status();

    return toHex(*bytes);
}

Result<std::string> digestHex(HashAlgorithm algorithm, std::string_view data)
{
    auto bytes = bytesFromString(data);
    return digestHex(algorithm, *bytes);
}

Result<Bytes> sign(
    HashAlgorithm algorithm,
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data)
{
    const auto* digest = digestFor(algorithm);
    if (digest == nullptr)
        return Status::invalidArgument("unsupported HMAC algorithm");
    if (key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Status::resourceExhausted("HMAC key is too large");

    Bytes out(static_cast<std::size_t>(EVP_MD_get_size(digest)));
    unsigned int outSize = 0;
    const auto* result = HMAC(
        digest,
        key.data(),
        static_cast<int>(key.size()),
        data.data(),
        data.size(),
        out.data(),
        &outSize);
    if (result == nullptr)
        return Status::internal("failed to compute HMAC");

    out.resize(outSize);
    return out;
}

Result<std::string> signHex(
    HashAlgorithm algorithm,
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data)
{
    auto signature = sign(algorithm, key, data);
    if (!signature.isOk())
        return signature.status();

    return toHex(*signature);
}

Result<std::string> signHex(HashAlgorithm algorithm, std::string_view key, std::string_view data)
{
    auto keyBytes = bytesFromString(key);
    auto dataBytes = bytesFromString(data);
    return signHex(algorithm, *keyBytes, *dataBytes);
}

std::string toHex(std::span<const std::uint8_t> bytes)
{
    std::string out(bytes.size() * 2, '0');
    std::size_t cursor = 0;
    for (const auto byte : bytes) {
        out[cursor++] = kHexAlphabet[(byte >> 4U) & 0x0FU];
        out[cursor++] = kHexAlphabet[byte & 0x0FU];
    }
    return out;
}

Result<Bytes> fromHex(std::string_view hex)
{
    if ((hex.size() % 2) != 0)
        return Status::invalidArgument("hex string must have even length");

    Bytes out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto high = hexValue(hex[i * 2]);
        const auto low = hexValue(hex[i * 2 + 1]);
        if (high == 0xff || low == 0xff)
            return Status::invalidArgument("hex string contains non-hex character");
        out[i] = static_cast<std::uint8_t>((high << 4U) | low);
    }

    return out;
}

std::string maskSecret(std::string_view value, const MaskOptions& options)
{
    if (value.empty())
        return {};

    const auto maskLength = std::max<std::size_t>(options.minMaskedLength_, 1);
    if (value.size() <= options.visiblePrefix_ + options.visibleSuffix_ + maskLength)
        return std::string(maskLength, options.maskChar_);

    const auto visiblePrefix = std::min(options.visiblePrefix_, value.size());
    const auto remaining = value.size() - visiblePrefix;
    const auto visibleSuffix = std::min(options.visibleSuffix_, remaining);

    std::string out;
    out.reserve(visiblePrefix + maskLength + visibleSuffix);
    out.append(value.substr(0, visiblePrefix));
    out.append(maskLength, options.maskChar_);
    if (visibleSuffix > 0)
        out.append(value.substr(value.size() - visibleSuffix));

    return out;
}

bool secureEquals(std::span<const std::uint8_t> lhs, std::span<const std::uint8_t> rhs) noexcept
{
    if (lhs.size() != rhs.size())
        return false;

    return CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

} // namespace lgc
