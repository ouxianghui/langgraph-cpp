#include "langgraph/graph/run_config.hpp"

#include <limits>
#include <string_view>
#include <utility>

namespace lc {
namespace {

[[nodiscard]] bool isRunnableConfigKey(std::string_view key) noexcept
{
    return key == "tags"
        || key == "metadata"
        || key == "callbacks"
        || key == "recursion_limit"
        || key == "max_concurrency"
        || key == "run_name"
        || key == "run_id"
        || key == "configurable";
}

[[nodiscard]] bool isTruthyConfigValue(const nlohmann::json& value)
{
    if (value.is_null())
        return false;
    if (value.is_boolean())
        return value.get<bool>();
    if (value.is_number_integer())
        return value.get<std::int64_t>() != 0;
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() != 0U;
    if (value.is_number_float())
        return value.get<double>() != 0.0;
    if (value.is_string())
        return !value.get_ref<const std::string&>().empty();
    if (value.is_array() || value.is_object())
        return !value.empty();
    return true;
}

[[nodiscard]] bool isPresentConfigValue(const nlohmann::json& value)
{
    if (value.is_null())
        return false;
    if (value.is_array() || value.is_object())
        return !value.empty();
    return true;
}

[[nodiscard]] bool isMetadataConfigValue(const nlohmann::json& value) noexcept
{
    return value.is_string()
        || value.is_number()
        || value.is_boolean();
}

[[nodiscard]] bool shouldPropagateConfigurableMetadata(
    std::string_view key,
    const nlohmann::json& value) noexcept
{
    return (key == "thread_id"
            || key == "checkpoint_id"
            || key == "checkpoint_ns"
            || key == "task_id"
            || key == "run_id"
            || key == "assistant_id"
            || key == "graph_id")
        && isMetadataConfigValue(value)
        && isTruthyConfigValue(value);
}

void mergeJsonObject(nlohmann::json& target, const nlohmann::json& source)
{
    if (!target.is_object())
        target = nlohmann::json::object();
    if (!source.is_object())
        return;
    for (const auto& item : source.items())
        target[item.key()] = item.value();
}

void mergeTags(std::vector<std::string>& target, const std::vector<std::string>& source)
{
    target.insert(target.end(), source.begin(), source.end());
}

[[nodiscard]] Result<std::vector<std::string>> parseRunnableTags(const nlohmann::json& value)
{
    if (!value.is_array())
        return Status::invalidArgument("RunnableConfig tags must be an array");
    std::vector<std::string> tags;
    tags.reserve(value.size());
    for (const auto& tag : value) {
        if (!tag.is_string())
            return Status::invalidArgument("RunnableConfig tag must be a string");
        tags.push_back(tag.get<std::string>());
    }
    return tags;
}

[[nodiscard]] Result<std::uint64_t> parseRunnableUint64(
    const nlohmann::json& value,
    std::string_view field)
{
    if (value.is_number_integer()) {
        const auto parsed = value.get<std::int64_t>();
        if (parsed < 0) {
            std::string message("RunnableConfig ");
            message.append(field);
            message.append(" cannot be negative");
            return Status::invalidArgument(std::move(message));
        }
        return static_cast<std::uint64_t>(parsed);
    }
    if (!value.is_number_unsigned()) {
        std::string message("RunnableConfig ");
        message.append(field);
        message.append(" must be an unsigned integer");
        return Status::invalidArgument(std::move(message));
    }
    return value.get<std::uint64_t>();
}

[[nodiscard]] Result<std::size_t> parseRunnableSize(
    const nlohmann::json& value,
    std::string_view field)
{
    auto parsed = parseRunnableUint64(value, field);
    if (!parsed.isOk())
        return parsed.status();
    if (*parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::string message("RunnableConfig ");
        message.append(field);
        message.append(" is too large");
        return Status::resourceExhausted(std::move(message));
    }
    return static_cast<std::size_t>(*parsed);
}

void propagateConfigurableMetadata(RunnableConfig& config)
{
    if (!config.metadata_.is_object())
        config.metadata_ = nlohmann::json::object();
    if (!config.configurable_.is_object())
        config.configurable_ = nlohmann::json::object();
    for (const auto& item : config.configurable_.items()) {
        if (!shouldPropagateConfigurableMetadata(item.key(), item.value()))
            continue;
        if (!config.metadata_.contains(item.key()))
            config.metadata_[item.key()] = item.value();
    }
}

[[nodiscard]] Result<void> applyRunnableConfigField(
    RunnableConfig& config,
    std::string_view key,
    const nlohmann::json& value)
{
    if (key == "tags") {
        auto tags = parseRunnableTags(value);
        if (!tags.isOk())
            return tags.status();
        config.tags_ = std::move(*tags);
        return okResult();
    }
    if (key == "metadata") {
        if (!value.is_object())
            return Status::invalidArgument("RunnableConfig metadata must be an object");
        config.metadata_ = value;
        return okResult();
    }
    if (key == "callbacks") {
        config.callbacks_ = value;
        return okResult();
    }
    if (key == "recursion_limit") {
        auto parsed = parseRunnableUint64(value, key);
        if (!parsed.isOk())
            return parsed.status();
        if (*parsed == 0U)
            return Status::invalidArgument("RunnableConfig recursion_limit must be positive");
        config.recursionLimit_ = *parsed;
        return okResult();
    }
    if (key == "max_concurrency") {
        auto parsed = parseRunnableSize(value, key);
        if (!parsed.isOk())
            return parsed.status();
        config.maxConcurrency_ = *parsed;
        return okResult();
    }
    if (key == "run_name") {
        if (!value.is_string())
            return Status::invalidArgument("RunnableConfig run_name must be a string");
        config.runName_ = value.get<std::string>();
        return okResult();
    }
    if (key == "run_id") {
        if (!value.is_string())
            return Status::invalidArgument("RunnableConfig run_id must be a string");
        config.runId_ = value.get<std::string>();
        return okResult();
    }
    if (key == "configurable") {
        if (!value.is_object())
            return Status::invalidArgument("RunnableConfig configurable must be an object");
        config.configurable_ = value;
        return okResult();
    }
    return Status::invalidArgument("unknown RunnableConfig field");
}

[[nodiscard]] nlohmann::json tagsToJson(const std::vector<std::string>& tags)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& tag : tags)
        out.push_back(tag);
    return out;
}

void mergeCallbacks(nlohmann::json& target, const nlohmann::json& source)
{
    if (source.is_null())
        return;
    if (target.is_null()) {
        target = source;
        return;
    }
    if (target.is_array() && source.is_array()) {
        for (const auto& item : source)
            target.push_back(item);
        return;
    }
    if (target.is_object() && source.is_object()) {
        mergeJsonObject(target, source);
        return;
    }
    target = source;
}

[[nodiscard]] Status validateRunOptionsConfig(const RunOptions& options)
{
    if (!options.metadata_.is_object())
        return Status::invalidArgument("RunOptions metadata must be an object");
    if (!options.configurable_.is_object())
        return Status::invalidArgument("RunOptions configurable must be an object");
    return Status::ok();
}

} // namespace

Command Command::resume(nlohmann::json value, StateUpdate update)
{
    return Command {
        .update_ = std::move(update),
        .resume_ = std::move(value),
    };
}

Command Command::gotoNode(NodeId target, StateUpdate update)
{
    std::vector<NodeId> targets;
    targets.push_back(std::move(target));
    return gotoNodes(std::move(targets), std::move(update));
}

Command Command::gotoNodes(std::vector<NodeId> targets, StateUpdate update)
{
    return Command {
        .graph_ = CommandGraph::Current,
        .update_ = std::move(update),
        .goto_ = std::move(targets),
    };
}

Command Command::gotoParentNode(NodeId target, StateUpdate update)
{
    std::vector<NodeId> targets;
    targets.push_back(std::move(target));
    return gotoParentNodes(std::move(targets), std::move(update));
}

Command Command::gotoParentNodes(std::vector<NodeId> targets, StateUpdate update)
{
    return Command {
        .graph_ = CommandGraph::Parent,
        .update_ = std::move(update),
        .goto_ = std::move(targets),
    };
}

RunOptions RunOptions::onlyEvents(std::set<RuntimeEventType> eventTypes)
{
    RunOptions options;
    options.eventTypes_ = std::move(eventTypes);
    return options;
}

RunOptions RunOptions::onlyEvents(std::initializer_list<RuntimeEventType> eventTypes)
{
    return onlyEvents(std::set<RuntimeEventType>(eventTypes));
}

RunOptions RunOptions::streamingDefaults()
{
    return RunOptions {};
}

RunOptions RunOptions::debugEvents()
{
    return onlyEvents({
        RuntimeEventType::RunStarted,
        RuntimeEventType::RunCompleted,
        RuntimeEventType::RunFailed,
        RuntimeEventType::NodeStarted,
        RuntimeEventType::NodeCompleted,
        RuntimeEventType::NodeFailed,
        RuntimeEventType::StateUpdated,
        RuntimeEventType::CheckpointCreated,
        RuntimeEventType::InterruptRequested,
        RuntimeEventType::Custom,
    });
}

Result<RunnableConfig> RunnableConfig::fromJson(const nlohmann::json& value)
{
    RunnableConfig config;
    if (value.is_null()) {
        propagateConfigurableMetadata(config);
        return config;
    }
    if (!value.is_object())
        return Status::invalidArgument("RunnableConfig must be an object");

    for (const auto& item : value.items()) {
        if (isRunnableConfigKey(item.key())) {
            if (auto status = applyRunnableConfigField(config, item.key(), item.value()); !status.isOk())
                return status.status();
            continue;
        }
        if (isPresentConfigValue(item.value()))
            config.configurable_[item.key()] = item.value();
    }
    propagateConfigurableMetadata(config);
    return config;
}

nlohmann::json RunnableConfig::toJson() const
{
    nlohmann::json out = nlohmann::json::object();
    if (!tags_.empty())
        out["tags"] = tagsToJson(tags_);
    if (metadata_.is_object() && !metadata_.empty())
        out["metadata"] = metadata_;
    if (!callbacks_.is_null())
        out["callbacks"] = callbacks_;
    if (recursionLimit_.has_value())
        out["recursion_limit"] = *recursionLimit_;
    if (maxConcurrency_.has_value())
        out["max_concurrency"] = *maxConcurrency_;
    if (!runName_.empty())
        out["run_name"] = runName_;
    if (!runId_.empty())
        out["run_id"] = runId_;
    out["configurable"] = configurable_.is_object() ? configurable_ : nlohmann::json::object();
    return out;
}

Result<RunnableConfig> mergeRunnableConfigs(std::vector<RunnableConfig> configs)
{
    RunnableConfig merged;
    for (auto& config : configs) {
        if (!config.metadata_.is_object())
            return Status::invalidArgument("RunnableConfig metadata must be an object");
        if (!config.configurable_.is_object())
            return Status::invalidArgument("RunnableConfig configurable must be an object");

        mergeTags(merged.tags_, config.tags_);
        mergeJsonObject(merged.metadata_, config.metadata_);
        mergeCallbacks(merged.callbacks_, config.callbacks_);
        if (config.recursionLimit_.has_value())
            merged.recursionLimit_ = config.recursionLimit_;
        if (config.maxConcurrency_.has_value())
            merged.maxConcurrency_ = config.maxConcurrency_;
        if (!config.runName_.empty())
            merged.runName_ = std::move(config.runName_);
        if (!config.runId_.empty())
            merged.runId_ = std::move(config.runId_);
        mergeJsonObject(merged.configurable_, config.configurable_);
    }
    propagateConfigurableMetadata(merged);
    return merged;
}

Result<RunnableConfig> patchRunnableConfig(
    RunnableConfig config,
    const nlohmann::json& patch)
{
    if (!patch.is_object())
        return Status::invalidArgument("RunnableConfig patch must be an object");
    if (!config.metadata_.is_object())
        return Status::invalidArgument("RunnableConfig metadata must be an object");
    if (!config.configurable_.is_object())
        return Status::invalidArgument("RunnableConfig configurable must be an object");

    for (const auto& item : patch.items()) {
        if (item.key() == "callbacks") {
            config.callbacks_ = item.value();
            config.runName_.clear();
            config.runId_.clear();
            continue;
        }
        if (isRunnableConfigKey(item.key())) {
            if (item.key() == "tags") {
                auto tags = parseRunnableTags(item.value());
                if (!tags.isOk())
                    return tags.status();
                mergeTags(config.tags_, *tags);
                continue;
            }
            if (item.key() == "metadata") {
                if (!item.value().is_object())
                    return Status::invalidArgument("RunnableConfig metadata must be an object");
                mergeJsonObject(config.metadata_, item.value());
                continue;
            }
            if (item.key() == "configurable") {
                if (!item.value().is_object())
                    return Status::invalidArgument("RunnableConfig configurable must be an object");
                mergeJsonObject(config.configurable_, item.value());
                continue;
            }
            if (auto status = applyRunnableConfigField(config, item.key(), item.value()); !status.isOk())
                return status.status();
            continue;
        }
        if (isPresentConfigValue(item.value()))
            config.configurable_[item.key()] = item.value();
    }
    propagateConfigurableMetadata(config);
    return config;
}

Result<RunOptions> applyRunnableConfig(
    RunOptions options,
    const RunnableConfig& config)
{
    if (!config.metadata_.is_object())
        return Status::invalidArgument("RunnableConfig metadata must be an object");
    if (!config.configurable_.is_object())
        return Status::invalidArgument("RunnableConfig configurable must be an object");
    if (auto status = validateRunOptionsConfig(options); !status.isOk())
        return status;

    auto normalized = config;
    propagateConfigurableMetadata(normalized);

    mergeTags(options.tags_, normalized.tags_);
    mergeJsonObject(options.metadata_, normalized.metadata_);
    mergeJsonObject(options.configurable_, normalized.configurable_);
    if (!normalized.callbacks_.is_null())
        options.callbacks_ = normalized.callbacks_;
    if (!normalized.runName_.empty())
        options.runName_ = normalized.runName_;
    if (!normalized.runId_.empty())
        options.runId_ = normalized.runId_;
    if (normalized.recursionLimit_.has_value())
        options.limits_ = options.limits_.maxSteps(*normalized.recursionLimit_);
    if (normalized.maxConcurrency_.has_value())
        options.maxConcurrency_ = *normalized.maxConcurrency_;

    if (normalized.configurable_.contains("thread_id")) {
        if (!normalized.configurable_.at("thread_id").is_string())
            return Status::invalidArgument("RunnableConfig configurable.thread_id must be a string");
        options.threadId_ = normalized.configurable_.at("thread_id").get<std::string>();
    }
    if (normalized.configurable_.contains("checkpoint_ns")) {
        if (!normalized.configurable_.at("checkpoint_ns").is_string())
            return Status::invalidArgument("RunnableConfig configurable.checkpoint_ns must be a string");
        options.checkpointNamespace_ = normalized.configurable_.at("checkpoint_ns").get<std::string>();
    }
    return options;
}

} // namespace lc
