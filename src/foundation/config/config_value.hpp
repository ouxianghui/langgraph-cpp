#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lgc {

enum class ConfigValueType : std::uint8_t {
    String,
    Bool,
    Int64,
    Double,
    StringList,
    BoolList,
    Int64List,
    DoubleList,
};

enum class ConfigSourceType : std::uint8_t {
    Manual,
    JsonString,
    JsonFile,
    Environment,
    Default,
};

struct ConfigSource {
    ConfigSourceType type_ { ConfigSourceType::Manual };
    std::string name_;
};

struct ConfigEntryMetadata {
    ConfigSource source_;
    bool sensitive_ { false };
};

class ConfigValue final {
public:
    using StringList = std::vector<std::string>;
    using BoolList = std::vector<bool>;
    using Int64List = std::vector<std::int64_t>;
    using DoubleList = std::vector<double>;

    ConfigValue() = default;
    ConfigValue(const char* value);
    ConfigValue(std::string value);
    ConfigValue(std::string_view value);
    ConfigValue(bool value);
    ConfigValue(std::int64_t value);
    ConfigValue(int value);
    ConfigValue(double value);
    ConfigValue(StringList value);
    ConfigValue(BoolList value);
    ConfigValue(Int64List value);
    ConfigValue(DoubleList value);

    [[nodiscard]] ConfigValueType type() const noexcept;

    [[nodiscard]] Result<std::string> asString() const;
    [[nodiscard]] Result<bool> asBool() const;
    [[nodiscard]] Result<std::int64_t> asInt64() const;
    [[nodiscard]] Result<double> asDouble() const;
    [[nodiscard]] Result<StringList> asStringList() const;
    [[nodiscard]] Result<BoolList> asBoolList() const;
    [[nodiscard]] Result<Int64List> asInt64List() const;
    [[nodiscard]] Result<DoubleList> asDoubleList() const;

private:
    using Storage = std::variant<std::string, bool, std::int64_t, double, StringList, BoolList, Int64List, DoubleList>;

    Storage value_ { std::string() };
};

class Config final {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const;

    [[nodiscard]] Result<void> set(
        std::string key,
        ConfigValue value,
        ConfigEntryMetadata metadata = {});
    [[nodiscard]] Result<void> merge(const Config& other, bool overwrite = true);

    [[nodiscard]] std::optional<ConfigValue> get(std::string_view key) const;
    [[nodiscard]] std::optional<ConfigEntryMetadata> metadata(std::string_view key) const;
    [[nodiscard]] bool sensitive(std::string_view key) const;
    [[nodiscard]] Result<ConfigValue> require(std::string_view key) const;

    [[nodiscard]] Result<std::string> stringValue(std::string_view key) const;
    [[nodiscard]] Result<bool> boolValue(std::string_view key) const;
    [[nodiscard]] Result<std::int64_t> int64Value(std::string_view key) const;
    [[nodiscard]] Result<double> doubleValue(std::string_view key) const;
    [[nodiscard]] Result<ConfigValue::StringList> stringListValue(std::string_view key) const;
    [[nodiscard]] Result<ConfigValue::BoolList> boolListValue(std::string_view key) const;
    [[nodiscard]] Result<ConfigValue::Int64List> int64ListValue(std::string_view key) const;
    [[nodiscard]] Result<ConfigValue::DoubleList> doubleListValue(std::string_view key) const;
    [[nodiscard]] Result<std::string> redactedStringValue(
        std::string_view key,
        std::string_view replacement = "[REDACTED]") const;

    [[nodiscard]] const std::unordered_map<std::string, ConfigValue>& values() const noexcept;

private:
    std::unordered_map<std::string, ConfigValue> values_;
    std::unordered_map<std::string, ConfigEntryMetadata> metadata_;
};

[[nodiscard]] Status validateConfigKey(std::string_view key);

} // namespace lgc
