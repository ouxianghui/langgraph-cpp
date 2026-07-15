#include "foundation/config/config_loader.hpp"

#include "foundation/config/env.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace lgc {
namespace {

using nlohmann::json;
constexpr std::string_view kJsonStringSourceName = "<json>";

struct FlattenState {
    std::size_t keys_ { 0 };
};

[[nodiscard]] std::string normalizeSensitiveKey(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0)
            out.push_back(static_cast<char>(std::tolower(byte)));
    }
    return out;
}

[[nodiscard]] bool containsAny(std::string_view haystack, const std::vector<std::string>& needles)
{
    return std::any_of(needles.begin(), needles.end(), [&](const std::string& needle) {
        return !needle.empty() && haystack.find(needle) != std::string_view::npos;
    });
}

[[nodiscard]] bool inferredSensitiveKey(std::string_view key)
{
    static const std::vector<std::string> sensitiveSubstrings {
        "apikey",
        "accesstoken",
        "refreshtoken",
        "idtoken",
        "authtoken",
        "authorization",
        "password",
        "clientsecret",
        "privatekey",
        "secret",
        "credential",
        "token",
    };
    return containsAny(normalizeSensitiveKey(key), sensitiveSubstrings);
}

[[nodiscard]] bool sensitiveKey(std::string_view key, const ConfigLoaderOptions& options)
{
    if (!options.inferSensitiveKeys_ && options.sensitiveKeys_.empty())
        return false;

    const auto normalized = normalizeSensitiveKey(key);
    if (options.inferSensitiveKeys_ && inferredSensitiveKey(key))
        return true;

    std::vector<std::string> normalizedConfigured;
    normalizedConfigured.reserve(options.sensitiveKeys_.size());
    for (const auto& item : options.sensitiveKeys_)
        normalizedConfigured.push_back(normalizeSensitiveKey(item));
    return containsAny(normalized, normalizedConfigured);
}

[[nodiscard]] std::string joinKey(std::string_view prefix, std::string_view key)
{
    if (prefix.empty())
        return std::string(key);
    if (key.empty())
        return std::string(prefix);

    std::string out;
    out.reserve(prefix.size() + 1 + key.size());
    out.append(prefix);
    out.push_back('.');
    out.append(key);
    return out;
}

[[nodiscard]] Result<std::int64_t> jsonInt64(const json& value)
{
    try {
        if (value.is_number_unsigned()) {
            const auto out = value.get<std::uint64_t>();
            if (out > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                return Status::outOfRange("config integer exceeds int64 range");
            return static_cast<std::int64_t>(out);
        }
        return value.get<std::int64_t>();
    } catch (const std::exception& error) {
        return Status::outOfRange(error.what());
    }
}

[[nodiscard]] Result<double> jsonDouble(const json& value)
{
    try {
        const auto out = value.get<double>();
        if (!std::isfinite(out))
            return Status::invalidArgument("config double must be finite");
        return out;
    } catch (const std::exception& error) {
        return Status::outOfRange(error.what());
    }
}

[[nodiscard]] Result<ConfigValue> configValueFromJson(const json& value, const ConfigLoaderOptions& options)
{
    if (value.is_string()) {
        auto text = value.get<std::string>();
        if (text.size() > options.maxStringBytes_)
            return Status::resourceExhausted("config string value is too large");
        return ConfigValue(std::move(text));
    }
    if (value.is_boolean())
        return ConfigValue(value.get<bool>());
    if (value.is_number_integer() || value.is_number_unsigned()) {
        auto parsed = jsonInt64(value);
        if (!parsed.isOk())
            return parsed.status();
        return ConfigValue(*parsed);
    }
    if (value.is_number_float()) {
        auto parsed = jsonDouble(value);
        if (!parsed.isOk())
            return parsed.status();
        return ConfigValue(*parsed);
    }
    if (value.is_array()) {
        if (value.empty()) {
            if (options.rejectEmptyArray_)
                return Status::invalidArgument("config arrays cannot be empty");
            return ConfigValue(ConfigValue::StringList {});
        }

        enum class ArrayKind {
            Unknown,
            String,
            Bool,
            Int64,
            Double,
        };
        ArrayKind kind = ArrayKind::Unknown;
        ConfigValue::StringList strings;
        ConfigValue::BoolList bools;
        ConfigValue::Int64List ints;
        ConfigValue::DoubleList doubles;
        strings.reserve(value.size());
        bools.reserve(value.size());
        ints.reserve(value.size());
        doubles.reserve(value.size());

        for (const auto& item : value) {
            if (item.is_null())
                return Status::invalidArgument("config arrays cannot contain null");
            if (item.is_object() || item.is_array())
                return Status::invalidArgument("config arrays must contain scalar values");

            ArrayKind itemKind = ArrayKind::Unknown;
            if (item.is_string()) {
                itemKind = ArrayKind::String;
                auto text = item.get<std::string>();
                if (text.size() > options.maxStringBytes_)
                    return Status::resourceExhausted("config string value is too large");
                strings.push_back(std::move(text));
            } else if (item.is_boolean()) {
                itemKind = ArrayKind::Bool;
                bools.push_back(item.get<bool>());
            } else if (item.is_number_integer() || item.is_number_unsigned()) {
                itemKind = ArrayKind::Int64;
                auto parsed = jsonInt64(item);
                if (!parsed.isOk())
                    return parsed.status();
                ints.push_back(*parsed);
                doubles.push_back(static_cast<double>(*parsed));
            } else if (item.is_number_float()) {
                itemKind = ArrayKind::Double;
                auto parsed = jsonDouble(item);
                if (!parsed.isOk())
                    return parsed.status();
                doubles.push_back(*parsed);
            }

            if (kind == ArrayKind::Unknown) {
                kind = itemKind;
                continue;
            }
            const bool numericMix = (kind == ArrayKind::Int64 || kind == ArrayKind::Double)
                && (itemKind == ArrayKind::Int64 || itemKind == ArrayKind::Double);
            if (kind != itemKind && !(numericMix && !options.rejectMixedArray_))
                return Status::invalidArgument("config arrays must be homogeneous");
            if (numericMix && itemKind == ArrayKind::Double)
                kind = ArrayKind::Double;
        }

        switch (kind) {
        case ArrayKind::String:
            return ConfigValue(std::move(strings));
        case ArrayKind::Bool:
            return ConfigValue(std::move(bools));
        case ArrayKind::Int64:
            return ConfigValue(std::move(ints));
        case ArrayKind::Double:
            return ConfigValue(std::move(doubles));
        case ArrayKind::Unknown:
            break;
        }
    }
    return Status::invalidArgument("config value must be a scalar or scalar array");
}

[[nodiscard]] Result<void> flattenJsonObject(
    Config& config,
    const json& object,
    std::string_view prefix,
    const ConfigLoaderOptions& options,
    const ConfigSource& source,
    FlattenState& state,
    std::size_t depth)
{
    if (depth > options.maxDepth_)
        return Status::resourceExhausted("config json exceeds maximum depth");
    if (!object.is_object())
        return Status::invalidArgument("config json root must be an object");

    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto key = joinKey(prefix, it.key());
        if (auto status = validateConfigKey(key); !status.isOk())
            return status;
        if (it->is_object()) {
            if (auto result = flattenJsonObject(config, *it, key, options, source, state, depth + 1); !result.isOk())
                return result.status();
            continue;
        }

        if (it->is_null()) {
            if (options.rejectNull_)
                return Status::invalidArgument("config null values are not allowed");
            continue;
        }
        if (!options.allowedKeys_.empty() && !options.allowedKeys_.contains(key))
            return Status::invalidArgument("unknown config key: " + key);

        if (++state.keys_ > options.maxKeys_)
            return Status::resourceExhausted("config contains too many keys");

        auto value = configValueFromJson(*it, options);
        if (!value.isOk())
            return value.status();

        ConfigEntryMetadata metadata {
            .source_ = source,
            .sensitive_ = sensitiveKey(key, options),
        };
        if (auto result = config.set(key, *value, std::move(metadata)); !result.isOk())
            return result.status();
    }

    return okResult();
}

[[nodiscard]] Result<json> parseJson(std::string_view jsonText, const ConfigLoaderOptions& options)
{
    if (jsonText.size() > options.maxJsonBytes_)
        return Status::resourceExhausted("config json is too large");

    bool duplicateKey = false;
    bool depthExceeded = false;
    std::vector<std::unordered_set<std::string>> keys;
    try {
        auto callback = [&](int depth, json::parse_event_t event, json& parsed) {
            if (static_cast<std::size_t>(depth) > options.maxDepth_) {
                depthExceeded = true;
                return false;
            }
            if (event == json::parse_event_t::object_start) {
                keys.emplace_back();
            } else if (event == json::parse_event_t::object_end) {
                if (!keys.empty())
                    keys.pop_back();
            } else if (event == json::parse_event_t::key && !keys.empty()) {
                const auto key = parsed.get<std::string>();
                if (!keys.back().insert(key).second)
                    duplicateKey = true;
            }
            return true;
        };
        auto parsed = json::parse(jsonText, callback);
        if (depthExceeded)
            return Status::resourceExhausted("config json exceeds maximum depth");
        if (duplicateKey && options.rejectDuplicateKeys_)
            return Status::invalidArgument("config json contains duplicate keys");
        if (!parsed.is_object())
            return Status::invalidArgument("config json root must be an object");
        return parsed;
    } catch (const std::exception& error) {
        std::string message("failed to parse config json: ");
        message.append(error.what());
        return Status::invalidArgument(std::move(message));
    }
}

[[nodiscard]] Result<std::string> readTextFile(const std::filesystem::path& path, const ConfigLoaderOptions& options)
{
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (!ec && fileSize > options.maxFileBytes_)
        return Status::resourceExhausted("config file is too large");

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        std::string message("failed to open config file: ");
        message.append(path.string());
        return Status::notFound(std::move(message));
    }

    std::string text;
    file.seekg(0, std::ios::end);
    const auto streamSize = file.tellg();
    if (streamSize > 0)
        text.reserve(static_cast<std::size_t>(streamSize));
    file.seekg(0, std::ios::beg);
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

    if (!file.eof() && file.fail()) {
        std::string message("failed to read config file: ");
        message.append(path.string());
        return Status::internal(std::move(message));
    }
    if (text.size() > options.maxFileBytes_)
        return Status::resourceExhausted("config file is too large");

    return text;
}

[[nodiscard]] Result<ConfigValue> convertEnvValue(
    std::string_view value,
    ConfigValueType type)
{
    ConfigValue raw(value);
    switch (type) {
    case ConfigValueType::String:
        return raw;
    case ConfigValueType::Bool: {
        auto parsed = raw.asBool();
        if (!parsed.isOk())
            return parsed.status();
        return ConfigValue(*parsed);
    }
    case ConfigValueType::Int64: {
        auto parsed = raw.asInt64();
        if (!parsed.isOk())
            return parsed.status();
        return ConfigValue(*parsed);
    }
    case ConfigValueType::Double: {
        auto parsed = raw.asDouble();
        if (!parsed.isOk())
            return parsed.status();
        return ConfigValue(*parsed);
    }
    case ConfigValueType::StringList:
    case ConfigValueType::BoolList:
    case ConfigValueType::Int64List:
    case ConfigValueType::DoubleList:
        return Status::invalidArgument("list environment bindings are not supported");
    }
    return Status::invalidArgument("unsupported config value type");
}

[[nodiscard]] Status validateBinding(const ConfigEnvBinding& binding)
{
    if (auto status = validateConfigKey(binding.key_); !status.isOk())
        return status;
    if (auto status = validateEnvName(binding.envName_); !status.isOk())
        return status;
    return Status::ok();
}

} // namespace

Result<Config> ConfigLoader::fromJsonString(
    std::string_view jsonText,
    std::string_view keyPrefix,
    const ConfigLoaderOptions& options)
{
    auto parsed = parseJson(jsonText, options);
    if (!parsed.isOk())
        return parsed.status();

    Config config;
    FlattenState state;
    const ConfigSource source {
        .type_ = ConfigSourceType::JsonString,
        .name_ = std::string(kJsonStringSourceName),
    };
    if (auto result = flattenJsonObject(config, *parsed, keyPrefix, options, source, state, 0); !result.isOk())
        return result.status();
    return config;
}

Result<Config> ConfigLoader::fromJsonFile(
    const std::filesystem::path& path,
    std::string_view keyPrefix,
    const ConfigLoaderOptions& options)
{
    auto text = readTextFile(path, options);
    if (!text.isOk())
        return text.status();

    auto parsed = parseJson(*text, options);
    if (!parsed.isOk())
        return parsed.status();

    Config config;
    FlattenState state;
    const ConfigSource source {
        .type_ = ConfigSourceType::JsonFile,
        .name_ = path.string(),
    };
    if (auto result = flattenJsonObject(config, *parsed, keyPrefix, options, source, state, 0); !result.isOk())
        return result.status();
    return config;
}

Result<Config> ConfigLoader::fromEnvironment(
    std::span<const ConfigEnvBinding> bindings,
    const ConfigLoaderOptions& options)
{
    Config config;
    if (auto result = mergeEnvironment(config, bindings, true, options); !result.isOk())
        return result.status();
    return config;
}

Result<void> ConfigLoader::mergeJsonString(
    Config& config,
    std::string_view jsonText,
    std::string_view keyPrefix,
    bool overwrite,
    const ConfigLoaderOptions& options)
{
    auto loaded = fromJsonString(jsonText, keyPrefix, options);
    if (!loaded.isOk())
        return loaded.status();
    return config.merge(*loaded, overwrite);
}

Result<void> ConfigLoader::mergeJsonFile(
    Config& config,
    const std::filesystem::path& path,
    std::string_view keyPrefix,
    bool overwrite,
    const ConfigLoaderOptions& options)
{
    auto loaded = fromJsonFile(path, keyPrefix, options);
    if (!loaded.isOk())
        return loaded.status();
    return config.merge(*loaded, overwrite);
}

Result<void> ConfigLoader::mergeEnvironment(
    Config& config,
    std::span<const ConfigEnvBinding> bindings,
    bool overwrite,
    const ConfigLoaderOptions& options)
{
    Config loaded;
    for (const auto& binding : bindings) {
        if (auto status = validateBinding(binding); !status.isOk())
            return status;

        auto value = Env::get(binding.envName_);
        if (!value.has_value()) {
            if (binding.defaultValue_.has_value()) {
                ConfigEntryMetadata metadata {
                    .source_ = ConfigSource {
                        .type_ = ConfigSourceType::Default,
                        .name_ = binding.envName_,
                    },
                    .sensitive_ = binding.sensitive_ || sensitiveKey(binding.key_, options),
                };
                if (auto result = loaded.set(binding.key_, *binding.defaultValue_, std::move(metadata)); !result.isOk())
                    return result.status();
                continue;
            }

            if (binding.required_) {
                std::string message("missing required environment variable: ");
                message.append(binding.envName_);
                return Status::notFound(std::move(message));
            }
            continue;
        }

        auto converted = convertEnvValue(*value, binding.type_);
        if (!converted.isOk()) {
            std::string message("invalid environment variable ");
            message.append(binding.envName_);
            message.append(": ");
            message.append(converted.status().message());
            return Status(converted.status().code(), std::move(message));
        }

        ConfigEntryMetadata metadata {
            .source_ = ConfigSource {
                .type_ = ConfigSourceType::Environment,
                .name_ = binding.envName_,
            },
            .sensitive_ = binding.sensitive_ || sensitiveKey(binding.key_, options),
        };
        if (auto result = loaded.set(binding.key_, *converted, std::move(metadata)); !result.isOk())
            return result.status();
    }

    return config.merge(loaded, overwrite);
}

} // namespace lgc
