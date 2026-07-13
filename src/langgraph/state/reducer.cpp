#include "langgraph/state/reducer.hpp"

#include "langgraph/message/message.hpp"

#include <unordered_map>
#include <utility>

namespace lc {
namespace {

[[nodiscard]] Result<void> applyAppend(
    nlohmann::json& state,
    const std::string& field,
    const nlohmann::json& value)
{
    auto& target = state[field];
    if (target.is_null())
        target = nlohmann::json::array();
    if (!target.is_array())
        return Status::invalidArgument("append reducer target must be an array: " + field);

    if (value.is_array()) {
        for (const auto& item : value)
            target.push_back(item);
    } else {
        target.push_back(value);
    }

    return okResult();
}

[[nodiscard]] Result<void> applyMergeObject(
    nlohmann::json& state,
    const std::string& field,
    const nlohmann::json& value)
{
    if (!value.is_object())
        return Status::invalidArgument("merge_object reducer update must be an object: " + field);

    auto& target = state[field];
    if (target.is_null())
        target = nlohmann::json::object();
    if (!target.is_object())
        return Status::invalidArgument("merge_object reducer target must be an object: " + field);

    for (auto it = value.begin(); it != value.end(); ++it)
        target[it.key()] = it.value();

    return okResult();
}

[[nodiscard]] Result<std::vector<BaseMessage>> messagesFromAddMessagesValue(const nlohmann::json& value)
{
    if (value.is_array())
        return messagesFromJson(value);
    if (value.is_object()) {
        auto message = baseMessageFromJson(value);
        if (!message.isOk())
            return message.status();
        return std::vector<BaseMessage> { std::move(*message) };
    }
    return Status::invalidArgument("add_messages reducer value must be a message or message array");
}

[[nodiscard]] Result<void> applyAddMessages(
    nlohmann::json& state,
    const std::string& field,
    const nlohmann::json& value)
{
    std::vector<BaseMessage> left;
    if (state.contains(field) && !state.at(field).is_null()) {
        auto parsedLeft = messagesFromAddMessagesValue(state.at(field));
        if (!parsedLeft.isOk())
            return parsedLeft.status();
        left = std::move(*parsedLeft);
    }

    auto right = messagesFromAddMessagesValue(value);
    if (!right.isOk())
        return right.status();

    std::unordered_map<std::string, std::size_t> indexById;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!left[index].id_.empty())
            indexById[left[index].id_] = index;
    }

    for (auto& message : *right) {
        if (!message.id_.empty()) {
            const auto found = indexById.find(message.id_);
            if (found != indexById.end()) {
                left[found->second] = std::move(message);
                continue;
            }
            indexById[message.id_] = left.size();
        }
        left.push_back(std::move(message));
    }

    state[field] = messagesToJson(left);
    return okResult();
}

} // namespace

std::string_view reducerKindName(ReducerKind kind) noexcept
{
    switch (kind) {
    case ReducerKind::Overwrite:
        return "overwrite";
    case ReducerKind::Append:
        return "append";
    case ReducerKind::AddMessages:
        return "add_messages";
    case ReducerKind::MergeObject:
        return "merge_object";
    }
    return "overwrite";
}

ReducerRegistry& ReducerRegistry::set(std::string field, ReducerKind kind)
{
    reducers_[std::move(field)] = kind;
    return *this;
}

ReducerRegistry& ReducerRegistry::set(std::string field, ReducerFunction reducer)
{
    if (reducer)
        customReducers_[std::move(field)] = std::move(reducer);
    return *this;
}

ReducerKind ReducerRegistry::reducerFor(std::string_view field) const
{
    const auto found = reducers_.find(std::string(field));
    if (found == reducers_.end())
        return ReducerKind::Overwrite;
    return found->second;
}

const ReducerFunction* ReducerRegistry::customReducerFor(std::string_view field) const
{
    const auto found = customReducers_.find(std::string(field));
    if (found == customReducers_.end())
        return nullptr;
    return &found->second;
}

const std::map<std::string, ReducerKind>& ReducerRegistry::reducers() const noexcept
{
    return reducers_;
}

Result<State> applyStateUpdate(
    const State& state,
    const StateUpdate& update,
    const ReducerRegistry& reducers)
{
    nlohmann::json merged = state.view();

    for (auto it = update.values().begin(); it != update.values().end(); ++it) {
        if (isOverwriteJson(it.value())) {
            auto overwrite = overwriteFromJson(it.value());
            if (!overwrite.isOk())
                return overwrite.status();
            merged[it.key()] = std::move(overwrite->value_);
            continue;
        }

        if (const auto* custom = reducers.customReducerFor(it.key())) {
            const auto current = merged.contains(it.key())
                ? merged.at(it.key())
                : nlohmann::json(nullptr);
            auto reduced = (*custom)(current, it.value());
            if (!reduced.isOk())
                return reduced.status();
            merged[it.key()] = std::move(*reduced);
            continue;
        }

        const auto reducer = reducers.reducerFor(it.key());
        switch (reducer) {
        case ReducerKind::Overwrite:
            merged[it.key()] = it.value();
            break;
        case ReducerKind::Append:
            if (auto result = applyAppend(merged, it.key(), it.value()); !result.isOk())
                return result.status();
            break;
        case ReducerKind::AddMessages:
            if (auto result = applyAddMessages(merged, it.key(), it.value()); !result.isOk())
                return result.status();
            break;
        case ReducerKind::MergeObject:
            if (auto result = applyMergeObject(merged, it.key(), it.value()); !result.isOk())
                return result.status();
            break;
        }
    }

    return State::fromJsonValue(merged);
}

} // namespace lc
