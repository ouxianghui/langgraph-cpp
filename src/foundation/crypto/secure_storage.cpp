#include "foundation/crypto/encryption.hpp"

#include "foundation/crypto/encryption_common.hh"

#include <stdexcept>
#include <utility>

namespace lgc {
namespace {
using namespace encryption_detail;

[[nodiscard]] Bytes aadFromStorageKey(const StorageKey& key)
{
    Bytes out;
    out.reserve(key.scope_.size() + 1 + key.key_.size());
    const auto scope = bytesFromString(key.scope_);
    const auto itemKey = bytesFromString(key.key_);
    out.insert(out.end(), scope.begin(), scope.end());
    out.push_back(0);
    out.insert(out.end(), itemKey.begin(), itemKey.end());
    return out;
}

[[nodiscard]] Result<std::string> encryptStorageValue(
    const IEncryptor& encryptor,
    std::string_view value,
    const StorageKey& key,
    std::string_view keyId)
{
    EncryptionOptions options {
        .keyId_ = std::string(keyId),
        .associatedData_ = aadFromStorageKey(key),
    };
    auto encrypted = encryptText(encryptor, value, options);
    if (!encrypted.isOk())
        return encrypted.status();
    return encodeEncrypted(*encrypted);
}

[[nodiscard]] Result<std::string> decryptStorageValue(
    const IEncryptor& encryptor,
    std::string_view value,
    const StorageKey& key)
{
    auto payload = decodeEncrypted(value);
    if (!payload.isOk())
        return payload.status();

    return decryptText(
        encryptor,
        *payload,
        DecryptionOptions {
            .associatedData_ = aadFromStorageKey(key),
        });
}

} // namespace

SecureStorage::SecureStorage(
    std::shared_ptr<IStorage> inner,
    std::shared_ptr<IEncryptor> encryptor,
    std::string keyId)
    : inner_(std::move(inner))
    , encryptor_(std::move(encryptor))
    , keyId_(std::move(keyId))
{
    if (!inner_)
        throw std::invalid_argument("SecureStorage requires an inner storage");
    if (!encryptor_)
        throw std::invalid_argument("SecureStorage requires an encryptor");
}

Result<void> SecureStorage::put(StorageKey key, std::string value, const StoragePutOptions& options)
{
    if (auto status = validateStorageKey(key); !status.isOk())
        return status;
    if (auto status = validateStoragePutOptions(options); !status.isOk())
        return status;

    auto encrypted = encryptStorageValue(*encryptor_, value, key, keyId_);
    if (!encrypted.isOk())
        return encrypted.status();
    return inner_->put(std::move(key), std::move(*encrypted), options);
}

Result<std::optional<StorageItem>> SecureStorage::get(const StorageKey& key)
{
    auto item = inner_->get(key);
    if (!item.isOk())
        return item.status();
    if (!item->has_value())
        return std::optional<StorageItem> {};

    auto decrypted = decryptStorageValue(*encryptor_, item->value().value_, item->value().key_);
    if (!decrypted.isOk())
        return decrypted.status();

    auto out = item->value();
    out.value_ = std::move(*decrypted);
    return std::optional<StorageItem>(std::move(out));
}

Result<StorageListResult> SecureStorage::list(const StorageListOptions& options)
{
    auto listed = inner_->list(options);
    if (!listed.isOk())
        return listed.status();

    for (auto& item : listed->items_) {
        auto decrypted = decryptStorageValue(*encryptor_, item.value_, item.key_);
        if (!decrypted.isOk())
            return decrypted.status();
        item.value_ = std::move(*decrypted);
    }
    return *listed;
}

Result<void> SecureStorage::remove(const StorageKey& key)
{
    return inner_->remove(key);
}

Result<void> SecureStorage::clearScope(std::string_view scope)
{
    return inner_->clearScope(scope);
}

Status SecureStorage::flush()
{
    return inner_->flush();
}

Status SecureStorage::close()
{
    return inner_->close();
}

bool SecureStorage::isClosed() const noexcept
{
    return inner_->isClosed();
}

} // namespace lgc
