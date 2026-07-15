#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

using StoreNamespace = std::vector<std::string>;

struct StoreItem {
    StoreNamespace namespace_;
    std::string key_;
    nlohmann::json value_ { nlohmann::json::object() };
    std::chrono::system_clock::time_point createdAt_;
    std::chrono::system_clock::time_point updatedAt_;
};

struct StoreSearchItem {
    StoreNamespace namespace_;
    std::string key_;
    nlohmann::json value_ { nlohmann::json::object() };
    std::chrono::system_clock::time_point createdAt_;
    std::chrono::system_clock::time_point updatedAt_;
    std::optional<double> score_;
};

struct StoreSearchOptions {
    StoreNamespace namespacePrefix_;
    std::optional<std::string> query_;
    std::optional<nlohmann::json> filter_;
    std::size_t limit_ { 10 };
    std::size_t offset_ { 0 };
};

struct StoreListNamespacesOptions {
    StoreNamespace prefix_;
    StoreNamespace suffix_;
    std::optional<std::size_t> maxDepth_;
    std::size_t limit_ { 100 };
    std::size_t offset_ { 0 };
};

enum class StoreNamespaceMatchType {
    Prefix,
    Suffix,
};

struct StoreNamespaceMatchCondition {
    StoreNamespaceMatchType matchType_ { StoreNamespaceMatchType::Prefix };
    StoreNamespace path_;
};

struct StoreGetOp {
    StoreNamespace namespace_;
    std::string key_;
    bool refreshTtl_ { true };
};

struct StoreSearchOp {
    StoreNamespace namespacePrefix_;
    std::optional<std::string> query_;
    std::optional<nlohmann::json> filter_;
    std::size_t limit_ { 10 };
    std::size_t offset_ { 0 };
    bool refreshTtl_ { true };
};

struct StorePutOp {
    StoreNamespace namespace_;
    std::string key_;
    /// Nullopt deletes the item, matching LangGraph's PutOp(value=None) delete semantics.
    std::optional<nlohmann::json> value_;
    std::optional<std::vector<std::string>> index_;
};

struct StoreListNamespacesOp {
    std::vector<StoreNamespaceMatchCondition> matchConditions_;
    std::optional<std::size_t> maxDepth_;
    std::size_t limit_ { 100 };
    std::size_t offset_ { 0 };
};

using StoreOp = std::variant<StoreGetOp, StoreSearchOp, StorePutOp, StoreListNamespacesOp>;
using StoreBatchResult = std::variant<
    std::monostate,
    StoreItem,
    std::vector<StoreSearchItem>,
    std::vector<StoreNamespace>>;

} // namespace lgc
