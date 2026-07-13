#pragma once

#include "foundation/config/config_value.hpp"
#include "foundation/status/result.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lc {

struct ConfigEnvBinding {
    std::string key_;
    std::string envName_;
    ConfigValueType type_ { ConfigValueType::String };
    bool required_ { false };
    bool sensitive_ { false };
    std::optional<ConfigValue> defaultValue_;
};

struct ConfigLoaderOptions {
    std::size_t maxFileBytes_ { 1024 * 1024 };
    std::size_t maxJsonBytes_ { 1024 * 1024 };
    std::size_t maxDepth_ { 32 };
    std::size_t maxKeys_ { 1024 };
    std::size_t maxStringBytes_ { 16 * 1024 };
    bool rejectNull_ { true };
    bool rejectEmptyArray_ { true };
    bool rejectMixedArray_ { true };
    bool rejectDuplicateKeys_ { true };
    bool inferSensitiveKeys_ { true };
    std::vector<std::string> sensitiveKeys_;
    std::unordered_set<std::string> allowedKeys_;
};

class ConfigLoader final {
public:
    [[nodiscard]] static Result<Config> fromJsonString(
        std::string_view jsonText,
        std::string_view keyPrefix = {},
        const ConfigLoaderOptions& options = {});

    [[nodiscard]] static Result<Config> fromJsonFile(
        const std::filesystem::path& path,
        std::string_view keyPrefix = {},
        const ConfigLoaderOptions& options = {});

    [[nodiscard]] static Result<Config> fromEnvironment(
        std::span<const ConfigEnvBinding> bindings,
        const ConfigLoaderOptions& options = {});

    [[nodiscard]] static Result<void> mergeJsonString(
        Config& config,
        std::string_view jsonText,
        std::string_view keyPrefix = {},
        bool overwrite = true,
        const ConfigLoaderOptions& options = {});

    [[nodiscard]] static Result<void> mergeJsonFile(
        Config& config,
        const std::filesystem::path& path,
        std::string_view keyPrefix = {},
        bool overwrite = true,
        const ConfigLoaderOptions& options = {});

    [[nodiscard]] static Result<void> mergeEnvironment(
        Config& config,
        std::span<const ConfigEnvBinding> bindings,
        bool overwrite = true,
        const ConfigLoaderOptions& options = {});
};

} // namespace lc
