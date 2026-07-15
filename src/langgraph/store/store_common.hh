#pragma once

#include "foundation/status/result.hpp"
#include "foundation/storage/i_storage.hpp"
#include "langgraph/store/storage_store.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc::detail {

struct ParsedStoreEnvelope {
    StoreNamespace namespace_;
    std::string key_;
    nlohmann::json value_;
    std::uint32_t schemaVersion_ { 0 };
};

struct StoredStoreItem {
    StorageKey storageKey_;
    StoreItem item_;
};

[[nodiscard]] std::string storeHexEncode(std::string_view value);
[[nodiscard]] Result<void> validateStoreNamespace(const StoreNamespace& nameSpace);
[[nodiscard]] Result<void> validateStoreNamespacePrefix(const StoreNamespace& nameSpace);
[[nodiscard]] Result<void> validateStoreNamespaceSuffix(const StoreNamespace& nameSpace);
[[nodiscard]] Result<void> validateStoreNamespaceMatchCondition(
    const StoreNamespaceMatchCondition& condition);
[[nodiscard]] Result<void> validateStoreKey(std::string_view key);
[[nodiscard]] bool storeNamespaceMatches(
    const StoreNamespace& nameSpace,
    const StoreNamespaceMatchCondition& condition) noexcept;
[[nodiscard]] bool storeNamespaceMatchesAll(
    const StoreNamespace& nameSpace,
    const std::vector<StoreNamespaceMatchCondition>& conditions) noexcept;
[[nodiscard]] Result<ParsedStoreEnvelope> parseStoreEnvelope(std::string_view value);
[[nodiscard]] nlohmann::json storeEnvelopeFromItem(
    const StoreNamespace& nameSpace,
    std::string_view key,
    nlohmann::json value);
[[nodiscard]] bool storeItemIsNewer(const StoreItem& lhs, const StoreItem& rhs);
[[nodiscard]] StoreItem storeItemFromEnvelope(
    ParsedStoreEnvelope envelope,
    const StorageItem& item);
[[nodiscard]] StoreSearchItem storeSearchItemFromItem(
    StoreItem item,
    std::optional<double> score = std::nullopt);
[[nodiscard]] Result<bool> storeItemMatchesFilter(
    const StoreItem& item,
    const std::optional<nlohmann::json>& filter);
[[nodiscard]] Result<void> validateStoreSearchOptions(const StoreSearchOptions& options);
[[nodiscard]] Result<std::vector<StoredStoreItem>> listStorageStoreItems(
    const std::shared_ptr<IStorage>& storage,
    const StorageStoreOptions& options,
    std::string_view keyPrefix);
[[nodiscard]] Result<std::vector<StorageKey>> listStorageStoreKeys(
    const std::shared_ptr<IStorage>& storage,
    const StorageStoreOptions& options,
    std::string_view keyPrefix);

} // namespace lgc::detail
