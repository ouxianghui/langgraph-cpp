#include "langgraph/store/store_common.hh"

#include "foundation/serialization/json_limits.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>

namespace lc {
namespace detail {

constexpr std::uint32_t kStoreItemSchemaVersion = 1;

[[nodiscard]] std::string storeHexEncode(std::string_view value)
{
    constexpr std::array<char, 16> digits {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string out;
    out.reserve(value.size() * 2);
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

[[nodiscard]] Result<void> validateStoreNamespaceParts(
    const StoreNamespace& nameSpace,
    bool allowEmpty,
    bool enforceRoot)
{
    if (!allowEmpty && nameSpace.empty())
        return Status::invalidArgument("store namespace cannot be empty");
    if (enforceRoot && !nameSpace.empty() && nameSpace.front() == "langgraph")
        return Status::invalidArgument("store namespace root cannot be langgraph");
    for (const auto& part : nameSpace) {
        if (part.empty())
            return Status::invalidArgument("store namespace part cannot be empty");
        if (part.find('.') != std::string::npos)
            return Status::invalidArgument("store namespace part cannot contain periods");
    }
    return okResult();
}

[[nodiscard]] Result<void> validateStoreNamespace(const StoreNamespace& nameSpace)
{
    return validateStoreNamespaceParts(nameSpace, false, true);
}

[[nodiscard]] Result<void> validateStoreNamespacePrefix(const StoreNamespace& nameSpace)
{
    return validateStoreNamespaceParts(nameSpace, true, true);
}

[[nodiscard]] Result<void> validateStoreNamespaceSuffix(const StoreNamespace& nameSpace)
{
    return validateStoreNamespaceParts(nameSpace, true, false);
}

[[nodiscard]] Result<void> validateStoreNamespaceMatchCondition(
    const StoreNamespaceMatchCondition& condition)
{
    switch (condition.matchType_) {
    case StoreNamespaceMatchType::Prefix:
        return validateStoreNamespacePrefix(condition.path_);
    case StoreNamespaceMatchType::Suffix:
        return validateStoreNamespaceSuffix(condition.path_);
    }
    return Status::invalidArgument("store namespace match type is invalid");
}

[[nodiscard]] Result<void> validateStoreKey(std::string_view key)
{
    if (key.empty())
        return Status::invalidArgument("store key cannot be empty");
    return okResult();
}

[[nodiscard]] bool storeNamespaceMatches(
    const StoreNamespace& nameSpace,
    const StoreNamespaceMatchCondition& condition) noexcept
{
    const auto& path = condition.path_;
    if (path.size() > nameSpace.size())
        return false;

    switch (condition.matchType_) {
    case StoreNamespaceMatchType::Prefix:
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (path[i] != "*" && path[i] != nameSpace[i])
                return false;
        }
        return true;
    case StoreNamespaceMatchType::Suffix:
        for (std::size_t i = 0; i < path.size(); ++i) {
            const auto& expected = path[path.size() - 1U - i];
            const auto& actual = nameSpace[nameSpace.size() - 1U - i];
            if (expected != "*" && expected != actual)
                return false;
        }
        return true;
    }
    return false;
}

[[nodiscard]] bool storeNamespaceMatchesAll(
    const StoreNamespace& nameSpace,
    const std::vector<StoreNamespaceMatchCondition>& conditions) noexcept
{
    for (const auto& condition : conditions) {
        if (!storeNamespaceMatches(nameSpace, condition))
            return false;
    }
    return true;
}

[[nodiscard]] nlohmann::json storeNamespaceToJson(const StoreNamespace& nameSpace)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& part : nameSpace)
        out.push_back(part);
    return out;
}

[[nodiscard]] Result<StoreNamespace> storeNamespaceFromJson(const nlohmann::json& value)
{
    if (!value.is_array())
        return Status::invalidArgument("stored store namespace must be an array");
    StoreNamespace nameSpace;
    nameSpace.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string())
            return Status::invalidArgument("stored store namespace parts must be strings");
        nameSpace.push_back(item.get<std::string>());
    }
    if (auto status = validateStoreNamespace(nameSpace); !status.isOk())
        return status.status();
    return nameSpace;
}

[[nodiscard]] Result<std::uint32_t> storeSchemaVersionFromJson(const nlohmann::json& value)
{
    if (!value.contains("schema_version"))
        return 0U;
    if (!value.at("schema_version").is_number_unsigned())
        return Status::invalidArgument("stored store item schema_version must be unsigned integer");
    const auto version = value.at("schema_version").get<std::uint64_t>();
    if (version > std::numeric_limits<std::uint32_t>::max())
        return Status::resourceExhausted("stored store item schema_version is too large");
    if (version > kStoreItemSchemaVersion)
        return Status::unimplemented("stored store item schema_version is newer than this runtime");
    return static_cast<std::uint32_t>(version);
}

[[nodiscard]] Result<ParsedStoreEnvelope> parseStoreEnvelope(std::string_view value)
{
    auto parsed = parseJsonWithLimits(value, "stored store item");
    if (!parsed.isOk())
        return parsed.status();
    if (!parsed->is_object())
        return Status::invalidArgument("stored store item must be an object");

    auto schemaVersion = storeSchemaVersionFromJson(*parsed);
    if (!schemaVersion.isOk())
        return schemaVersion.status();
    if (!parsed->contains("namespace"))
        return Status::invalidArgument("stored store item namespace is required");
    if (!parsed->contains("key") || !parsed->at("key").is_string())
        return Status::invalidArgument("stored store item key is required");
    if (!parsed->contains("value"))
        return Status::invalidArgument("stored store item value is required");

    auto nameSpace = storeNamespaceFromJson(parsed->at("namespace"));
    if (!nameSpace.isOk())
        return nameSpace.status();
    auto key = parsed->at("key").get<std::string>();
    if (auto status = validateStoreKey(key); !status.isOk())
        return status.status();

    return ParsedStoreEnvelope {
        .namespace_ = std::move(*nameSpace),
        .key_ = std::move(key),
        .value_ = parsed->at("value"),
        .schemaVersion_ = *schemaVersion,
    };
}

[[nodiscard]] nlohmann::json storeEnvelopeFromItem(
    const StoreNamespace& nameSpace,
    std::string_view key,
    nlohmann::json value)
{
    nlohmann::json envelope = nlohmann::json::object();
    envelope["schema_version"] = kStoreItemSchemaVersion;
    envelope["namespace"] = storeNamespaceToJson(nameSpace);
    envelope["key"] = std::string(key);
    envelope["value"] = std::move(value);
    return envelope;
}

[[nodiscard]] bool storeItemIsNewer(const StoreItem& lhs, const StoreItem& rhs)
{
    if (lhs.updatedAt_ != rhs.updatedAt_)
        return lhs.updatedAt_ > rhs.updatedAt_;
    if (lhs.createdAt_ != rhs.createdAt_)
        return lhs.createdAt_ > rhs.createdAt_;
    if (lhs.namespace_ != rhs.namespace_)
        return lhs.namespace_ < rhs.namespace_;
    return lhs.key_ < rhs.key_;
}

[[nodiscard]] StoreItem storeItemFromEnvelope(
    ParsedStoreEnvelope envelope,
    const StorageItem& item)
{
    return StoreItem {
        .namespace_ = std::move(envelope.namespace_),
        .key_ = std::move(envelope.key_),
        .value_ = std::move(envelope.value_),
        .createdAt_ = item.createdAt_,
        .updatedAt_ = item.updatedAt_,
    };
}

[[nodiscard]] StoreSearchItem storeSearchItemFromItem(
    StoreItem item,
    std::optional<double> score)
{
    return StoreSearchItem {
        .namespace_ = std::move(item.namespace_),
        .key_ = std::move(item.key_),
        .value_ = std::move(item.value_),
        .createdAt_ = item.createdAt_,
        .updatedAt_ = item.updatedAt_,
        .score_ = score,
    };
}

[[nodiscard]] Result<bool> compareStoreFilterValue(
    const nlohmann::json& itemValue,
    const nlohmann::json& filterValue);

[[nodiscard]] Result<bool> applyStoreFilterOperator(
    const nlohmann::json& itemValue,
    std::string_view op,
    const nlohmann::json& expected)
{
    if (op == "$eq")
        return itemValue == expected;
    if (op == "$ne")
        return itemValue != expected;

    if (!itemValue.is_number() || !expected.is_number())
        return false;

    const auto actualNumber = itemValue.get<double>();
    const auto expectedNumber = expected.get<double>();
    if (op == "$gt")
        return actualNumber > expectedNumber;
    if (op == "$gte")
        return actualNumber >= expectedNumber;
    if (op == "$lt")
        return actualNumber < expectedNumber;
    if (op == "$lte")
        return actualNumber <= expectedNumber;

    return Status::invalidArgument("unsupported store filter operator: " + std::string(op));
}

[[nodiscard]] bool isStoreFilterOperatorObject(const nlohmann::json& value)
{
    if (!value.is_object())
        return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key().starts_with('$'))
            return true;
    }
    return false;
}

[[nodiscard]] Result<bool> compareStoreFilterObject(
    const nlohmann::json& itemValue,
    const nlohmann::json& filterValue)
{
    if (!itemValue.is_object())
        return false;

    for (const auto& [key, expected] : filterValue.items()) {
        if (key.starts_with('$'))
            return Status::invalidArgument("store filter operator cannot be mixed with field names");
        const auto found = itemValue.find(key);
        const nlohmann::json missing;
        auto matched = compareStoreFilterValue(
            found == itemValue.end() ? missing : *found,
            expected);
        if (!matched.isOk())
            return matched.status();
        if (!*matched)
            return false;
    }
    return true;
}

[[nodiscard]] Result<bool> compareStoreFilterArray(
    const nlohmann::json& itemValue,
    const nlohmann::json& filterValue)
{
    if (!itemValue.is_array() || itemValue.size() != filterValue.size())
        return false;

    for (std::size_t i = 0; i < filterValue.size(); ++i) {
        auto matched = compareStoreFilterValue(itemValue.at(i), filterValue.at(i));
        if (!matched.isOk())
            return matched.status();
        if (!*matched)
            return false;
    }
    return true;
}

[[nodiscard]] Result<bool> compareStoreFilterValue(
    const nlohmann::json& itemValue,
    const nlohmann::json& filterValue)
{
    if (isStoreFilterOperatorObject(filterValue)) {
        for (const auto& [op, expected] : filterValue.items()) {
            auto matched = applyStoreFilterOperator(itemValue, op, expected);
            if (!matched.isOk())
                return matched.status();
            if (!*matched)
                return false;
        }
        return true;
    }

    if (filterValue.is_object())
        return compareStoreFilterObject(itemValue, filterValue);
    if (filterValue.is_array())
        return compareStoreFilterArray(itemValue, filterValue);
    return itemValue == filterValue;
}

[[nodiscard]] Result<bool> storeItemMatchesFilter(
    const StoreItem& item,
    const std::optional<nlohmann::json>& filter)
{
    if (!filter.has_value())
        return true;
    if (!filter->is_object())
        return Status::invalidArgument("store search filter must be a JSON object");

    for (const auto& [key, expected] : filter->items()) {
        const auto found = item.value_.find(key);
        const nlohmann::json missing;
        auto matched = compareStoreFilterValue(
            found == item.value_.end() ? missing : *found,
            expected);
        if (!matched.isOk())
            return matched.status();
        if (!*matched)
            return false;
    }
    return true;
}

[[nodiscard]] Result<void> validateStoreSearchOptions(const StoreSearchOptions& options)
{
    if (auto status = validateStoreNamespacePrefix(options.namespacePrefix_); !status.isOk())
        return status.status();
    if (options.query_.has_value())
        return Status::unimplemented("store semantic search query is not supported by this store");
    if (options.filter_.has_value() && !options.filter_->is_object())
        return Status::invalidArgument("store search filter must be a JSON object");
    return okResult();
}

[[nodiscard]] Result<std::vector<StoredStoreItem>> listStorageStoreItems(
    const std::shared_ptr<IStorage>& storage,
    const StorageStoreOptions& options,
    std::string_view keyPrefix)
{
    std::vector<StoredStoreItem> out;
    std::string cursor;
    for (;;) {
        auto page = storage->list(StorageListOptions {
            .scope_ = options.scope_,
            .keyPrefix_ = std::string(keyPrefix),
            .limit_ = options.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();
        for (const auto& item : page->items_) {
            auto envelope = parseStoreEnvelope(item.value_);
            if (!envelope.isOk())
                return envelope.status();
            out.push_back(StoredStoreItem {
                .storageKey_ = item.key_,
                .item_ = storeItemFromEnvelope(std::move(*envelope), item),
            });
        }
        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }
    return out;
}

[[nodiscard]] Result<std::vector<StorageKey>> listStorageStoreKeys(
    const std::shared_ptr<IStorage>& storage,
    const StorageStoreOptions& options,
    std::string_view keyPrefix)
{
    std::vector<StorageKey> out;
    std::string cursor;
    for (;;) {
        auto page = storage->list(StorageListOptions {
            .scope_ = options.scope_,
            .keyPrefix_ = std::string(keyPrefix),
            .limit_ = options.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();
        for (const auto& item : page->items_)
            out.push_back(item.key_);
        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }
    return out;
}

} // namespace detail

} // namespace lc
