#include "foundation/config/config_value.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

namespace lgc {
namespace {

constexpr std::size_t kMaxConfigKeyLength = 256;

[[nodiscard]] bool validConfigKeyChar(unsigned char ch) noexcept
{
    return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
}

[[nodiscard]] std::string lowerAscii(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] std::string doubleToString(double value)
{
    std::ostringstream out;
    out << std::setprecision(15) << value;
    return out.str();
}

[[nodiscard]] Result<std::int64_t> parseInt64(std::string_view value)
{
    std::int64_t out = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc {} || result.ptr != end)
        return Status::invalidArgument("config value is not a valid int64");
    return out;
}

[[nodiscard]] Result<double> parseDouble(std::string_view value)
{
    std::string text(value);
    char* end = nullptr;
    errno = 0;
    const double out = std::strtod(text.c_str(), &end);
    if (errno != 0 || end != text.c_str() + text.size() || !std::isfinite(out))
        return Status::invalidArgument("config value is not a valid double");
    return out;
}

[[nodiscard]] Result<bool> parseBool(std::string_view value)
{
    const auto text = lowerAscii(std::string(value));
    if (text == "true" || text == "1" || text == "yes" || text == "y" || text == "on")
        return true;
    if (text == "false" || text == "0" || text == "no" || text == "n" || text == "off")
        return false;
    return Status::invalidArgument("config value is not a valid bool");
}

[[nodiscard]] std::string missingKeyMessage(std::string_view key)
{
    std::string message("missing config key: ");
    message.append(key);
    return message;
}

} // namespace

ConfigValue::ConfigValue(const char* value)
    : value_(std::string(value == nullptr ? "" : value))
{
}

ConfigValue::ConfigValue(std::string value)
    : value_(std::move(value))
{
}

ConfigValue::ConfigValue(std::string_view value)
    : value_(std::string(value))
{
}

ConfigValue::ConfigValue(bool value)
    : value_(value)
{
}

ConfigValue::ConfigValue(std::int64_t value)
    : value_(value)
{
}

ConfigValue::ConfigValue(int value)
    : value_(static_cast<std::int64_t>(value))
{
}

ConfigValue::ConfigValue(double value)
    : value_(value)
{
}

ConfigValue::ConfigValue(StringList value)
    : value_(std::move(value))
{
}

ConfigValue::ConfigValue(BoolList value)
    : value_(std::move(value))
{
}

ConfigValue::ConfigValue(Int64List value)
    : value_(std::move(value))
{
}

ConfigValue::ConfigValue(DoubleList value)
    : value_(std::move(value))
{
}

ConfigValueType ConfigValue::type() const noexcept
{
    if (std::holds_alternative<bool>(value_))
        return ConfigValueType::Bool;
    if (std::holds_alternative<std::int64_t>(value_))
        return ConfigValueType::Int64;
    if (std::holds_alternative<double>(value_))
        return ConfigValueType::Double;
    if (std::holds_alternative<StringList>(value_))
        return ConfigValueType::StringList;
    if (std::holds_alternative<BoolList>(value_))
        return ConfigValueType::BoolList;
    if (std::holds_alternative<Int64List>(value_))
        return ConfigValueType::Int64List;
    if (std::holds_alternative<DoubleList>(value_))
        return ConfigValueType::DoubleList;
    return ConfigValueType::String;
}

Result<std::string> ConfigValue::asString() const
{
    if (const auto* value = std::get_if<std::string>(&value_))
        return *value;
    if (const auto* value = std::get_if<bool>(&value_))
        return *value ? std::string("true") : std::string("false");
    if (const auto* value = std::get_if<std::int64_t>(&value_))
        return std::to_string(*value);
    if (const auto* value = std::get_if<double>(&value_))
        return doubleToString(*value);
    return Status::invalidArgument("config value is a list");
}

Result<bool> ConfigValue::asBool() const
{
    if (const auto* value = std::get_if<bool>(&value_))
        return *value;
    if (const auto* value = std::get_if<std::string>(&value_))
        return parseBool(*value);
    return Status::invalidArgument("config value is not a bool");
}

Result<std::int64_t> ConfigValue::asInt64() const
{
    if (const auto* value = std::get_if<std::int64_t>(&value_))
        return *value;
    if (const auto* value = std::get_if<std::string>(&value_))
        return parseInt64(*value);
    return Status::invalidArgument("config value is not an int64");
}

Result<double> ConfigValue::asDouble() const
{
    if (const auto* value = std::get_if<double>(&value_))
        return *value;
    if (const auto* value = std::get_if<std::int64_t>(&value_))
        return static_cast<double>(*value);
    if (const auto* value = std::get_if<std::string>(&value_))
        return parseDouble(*value);
    return Status::invalidArgument("config value is not a double");
}

Result<ConfigValue::StringList> ConfigValue::asStringList() const
{
    if (const auto* value = std::get_if<StringList>(&value_))
        return *value;
    return Status::invalidArgument("config value is not a string list");
}

Result<ConfigValue::BoolList> ConfigValue::asBoolList() const
{
    if (const auto* value = std::get_if<BoolList>(&value_))
        return *value;
    return Status::invalidArgument("config value is not a bool list");
}

Result<ConfigValue::Int64List> ConfigValue::asInt64List() const
{
    if (const auto* value = std::get_if<Int64List>(&value_))
        return *value;
    return Status::invalidArgument("config value is not an int64 list");
}

Result<ConfigValue::DoubleList> ConfigValue::asDoubleList() const
{
    if (const auto* value = std::get_if<DoubleList>(&value_))
        return *value;
    if (const auto* value = std::get_if<Int64List>(&value_)) {
        DoubleList out;
        out.reserve(value->size());
        for (const auto item : *value)
            out.push_back(static_cast<double>(item));
        return out;
    }
    return Status::invalidArgument("config value is not a double list");
}

bool Config::empty() const noexcept
{
    return values_.empty();
}

std::size_t Config::size() const noexcept
{
    return values_.size();
}

bool Config::contains(std::string_view key) const
{
    return values_.contains(std::string(key));
}

Result<void> Config::set(std::string key, ConfigValue value, ConfigEntryMetadata metadata)
{
    if (auto status = validateConfigKey(key); !status.isOk())
        return status;
    const auto storedKey = key;
    values_[std::move(key)] = std::move(value);
    metadata_[storedKey] = std::move(metadata);
    return okResult();
}

Result<void> Config::merge(const Config& other, bool overwrite)
{
    for (const auto& [key, value] : other.values_) {
        if (overwrite || !values_.contains(key)) {
            values_[key] = value;
            if (const auto it = other.metadata_.find(key); it != other.metadata_.end())
                metadata_[key] = it->second;
            else
                metadata_.erase(key);
        }
    }
    return okResult();
}

std::optional<ConfigValue> Config::get(std::string_view key) const
{
    const auto it = values_.find(std::string(key));
    if (it == values_.end())
        return std::nullopt;
    return it->second;
}

std::optional<ConfigEntryMetadata> Config::metadata(std::string_view key) const
{
    const auto it = metadata_.find(std::string(key));
    if (it == metadata_.end())
        return std::nullopt;
    return it->second;
}

bool Config::sensitive(std::string_view key) const
{
    const auto info = metadata(key);
    return info.has_value() && info->sensitive_;
}

Result<ConfigValue> Config::require(std::string_view key) const
{
    auto value = get(key);
    if (!value.has_value())
        return Status::notFound(missingKeyMessage(key));
    return *value;
}

Result<std::string> Config::stringValue(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asString();
}

Result<bool> Config::boolValue(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asBool();
}

Result<std::int64_t> Config::int64Value(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asInt64();
}

Result<double> Config::doubleValue(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asDouble();
}

Result<ConfigValue::StringList> Config::stringListValue(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asStringList();
}

Result<ConfigValue::BoolList> Config::boolListValue(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asBoolList();
}

Result<ConfigValue::Int64List> Config::int64ListValue(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asInt64List();
}

Result<ConfigValue::DoubleList> Config::doubleListValue(std::string_view key) const
{
    auto value = require(key);
    if (!value.isOk())
        return value.status();
    return value->asDoubleList();
}

Result<std::string> Config::redactedStringValue(std::string_view key, std::string_view replacement) const
{
    if (sensitive(key))
        return std::string(replacement);
    return stringValue(key);
}

const std::unordered_map<std::string, ConfigValue>& Config::values() const noexcept
{
    return values_;
}

Status validateConfigKey(std::string_view key)
{
    if (key.empty())
        return Status::invalidArgument("config key cannot be empty");
    if (key.size() > kMaxConfigKeyLength)
        return Status::invalidArgument("config key is too long");
    if (key.front() == '.' || key.back() == '.')
        return Status::invalidArgument("config key cannot start or end with '.'");
    if (key.find("..") != std::string_view::npos)
        return Status::invalidArgument("config key cannot contain '..'");
    for (const auto ch : key) {
        if (!validConfigKeyChar(static_cast<unsigned char>(ch)))
            return Status::invalidArgument("config key contains invalid characters");
    }
    return Status::ok();
}

} // namespace lgc
