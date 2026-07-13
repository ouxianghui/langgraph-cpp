#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

struct StorageKey {
    std::string scope_;
    std::string key_;

    friend bool operator==(const StorageKey&, const StorageKey&) = default;
};

enum class StoragePutMode : std::uint8_t {
    Upsert,
    InsertOnly,
    ReplaceOnly,
};

struct StoragePutOptions {
    StoragePutMode mode_ { StoragePutMode::Upsert };
    std::optional<std::uint64_t> expectedVersion_;
};

struct StorageItem {
    StorageKey key_;
    std::string value_;
    std::chrono::system_clock::time_point createdAt_;
    std::chrono::system_clock::time_point updatedAt_;
    std::uint64_t version_ { 0 };
};

struct StorageListOptions {
    std::string scope_;
    std::string keyPrefix_;
    std::size_t limit_ { 100 };
    std::string cursor_;
};

struct StorageListResult {
    std::vector<StorageItem> items_;
    std::string nextCursor_;
};

struct StorageLimits {
    std::size_t maxScopeBytes_ { 256 };
    std::size_t maxKeyBytes_ { 1024 };
    std::size_t maxValueBytes_ { 64 * 1024 * 1024 };
    std::size_t maxListItems_ { 1000 };
    std::size_t maxItems_ { 100'000 };
};

class IStorage {
public:
    virtual ~IStorage() = default;

    [[nodiscard]] virtual Result<void> put(
        StorageKey key,
        std::string value,
        const StoragePutOptions& options = {}) = 0;
    [[nodiscard]] virtual Result<std::optional<StorageItem>> get(const StorageKey& key) = 0;
    [[nodiscard]] virtual Result<StorageListResult> list(const StorageListOptions& options = {}) = 0;
    [[nodiscard]] virtual Result<void> remove(const StorageKey& key) = 0;
    [[nodiscard]] virtual Result<void> clearScope(std::string_view scope) = 0;
    [[nodiscard]] virtual Status flush() = 0;
    [[nodiscard]] virtual Status close() = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

[[nodiscard]] inline bool validStorageNameChar(unsigned char ch) noexcept
{
    return std::isalnum(ch) != 0
        || ch == '_'
        || ch == '-'
        || ch == '.'
        || ch == '/'
        || ch == ':'
        || ch == '@'
        || ch == '=';
}

[[nodiscard]] inline Status validateStorageName(
    std::string_view value,
    std::string_view label,
    std::size_t maxBytes)
{
    if (value.empty())
        return Status::invalidArgument(std::string(label) + " cannot be empty");
    if (maxBytes != 0U && value.size() > maxBytes)
        return Status::invalidArgument(std::string(label) + " is too long");
    for (const auto ch : value) {
        if (!validStorageNameChar(static_cast<unsigned char>(ch)))
            return Status::invalidArgument(std::string(label) + " contains invalid characters");
    }
    return Status::ok();
}

[[nodiscard]] inline Status validateStorageKey(const StorageKey& key, const StorageLimits& limits = {})
{
    if (auto status = validateStorageName(key.scope_, "storage scope", limits.maxScopeBytes_); !status.isOk())
        return status;
    if (auto status = validateStorageName(key.key_, "storage key", limits.maxKeyBytes_); !status.isOk())
        return status;
    return Status::ok();
}

[[nodiscard]] inline Status validateStorageValue(std::string_view value, const StorageLimits& limits = {})
{
    if (limits.maxValueBytes_ != 0U && value.size() > limits.maxValueBytes_)
        return Status::resourceExhausted("storage value is too large");
    return Status::ok();
}

[[nodiscard]] inline Status validateStorageListOptions(
    const StorageListOptions& options,
    const StorageLimits& limits = {})
{
    if (!options.scope_.empty()) {
        if (auto status = validateStorageName(options.scope_, "storage scope", limits.maxScopeBytes_); !status.isOk())
            return status;
    }
    if (!options.keyPrefix_.empty()) {
        if (auto status = validateStorageName(options.keyPrefix_, "storage key prefix", limits.maxKeyBytes_); !status.isOk())
            return status;
    }
    if (options.limit_ == 0U)
        return Status::invalidArgument("storage list limit must be greater than zero");
    if (limits.maxListItems_ != 0U && options.limit_ > limits.maxListItems_)
        return Status::resourceExhausted("storage list limit is too large");
    return Status::ok();
}

[[nodiscard]] inline Status validateStoragePutOptions(const StoragePutOptions& options)
{
    if (options.mode_ == StoragePutMode::InsertOnly && options.expectedVersion_.has_value())
        return Status::invalidArgument("insert-only storage put cannot use expectedVersion");
    return Status::ok();
}

[[nodiscard]] inline bool storageKeyMatchesListOptions(
    const StorageKey& key,
    const StorageListOptions& options)
{
    if (!options.scope_.empty() && key.scope_ != options.scope_)
        return false;
    if (!options.keyPrefix_.empty() && !key.key_.starts_with(options.keyPrefix_))
        return false;
    return true;
}

[[nodiscard]] inline std::string storageCursorFromKey(const StorageKey& key)
{
    std::string cursor;
    cursor.reserve(24 + key.scope_.size() + key.key_.size());
    cursor.append(std::to_string(key.scope_.size()));
    cursor.push_back(':');
    cursor.append(key.scope_);
    cursor.append(key.key_);
    return cursor;
}

[[nodiscard]] inline Result<std::optional<StorageKey>> storageKeyFromCursor(
    std::string_view cursor,
    const StorageLimits& limits = {})
{
    if (cursor.empty())
        return std::optional<StorageKey> {};

    const auto separator = cursor.find(':');
    if (separator == std::string_view::npos)
        return Status::invalidArgument("invalid storage cursor");

    std::size_t scopeSize = 0;
    const auto sizeText = cursor.substr(0, separator);
    const auto* begin = sizeText.data();
    const auto* end = begin + sizeText.size();
    const auto result = std::from_chars(begin, end, scopeSize);
    if (result.ec != std::errc {} || result.ptr != end)
        return Status::invalidArgument("invalid storage cursor");

    const auto payload = cursor.substr(separator + 1);
    if (scopeSize == 0 || scopeSize >= payload.size())
        return Status::invalidArgument("invalid storage cursor");

    StorageKey key {
        .scope_ = std::string(payload.substr(0, scopeSize)),
        .key_ = std::string(payload.substr(scopeSize)),
    };
    if (auto status = validateStorageKey(key, limits); !status.isOk())
        return status;

    return std::optional<StorageKey>(std::move(key));
}

} // namespace lc
