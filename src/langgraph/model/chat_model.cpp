#include "langgraph/model/chat_model.hpp"

#include <utility>

namespace lgc {
namespace {

[[nodiscard]] nlohmann::json toolCallChunkToJson(const ToolCallChunk& chunk)
{
    nlohmann::json value {
        { "type", "tool_call_chunk" },
        { "id", chunk.id_.empty() ? nlohmann::json(nullptr) : nlohmann::json(chunk.id_) },
        { "name", chunk.name_.empty() ? nlohmann::json(nullptr) : nlohmann::json(chunk.name_) },
        { "args", chunk.argumentsDelta_ },
        { "index", chunk.index_.has_value() ? nlohmann::json(*chunk.index_) : nlohmann::json(nullptr) },
    };
    return value;
}

[[nodiscard]] nlohmann::json toolCallChunksToJson(const std::vector<ToolCallChunk>& chunks)
{
    auto value = nlohmann::json::array();
    for (const auto& chunk : chunks)
        value.push_back(toolCallChunkToJson(chunk));
    return value;
}

[[nodiscard]] nlohmann::json contentBlocksForChunk(const AIMessageChunk& chunk)
{
    nlohmann::json blocks = chunk.contentBlocks_.is_array() && !chunk.contentBlocks_.empty()
        ? chunk.contentBlocks_
        : contentBlocksFromText(chunk.text_);
    bool hasToolCallChunkBlock = false;
    for (const auto& block : blocks) {
        if (block.is_object()
            && block.contains("type")
            && block.at("type") == "tool_call_chunk") {
            hasToolCallChunkBlock = true;
            break;
        }
    }
    if (!hasToolCallChunkBlock) {
        for (const auto& toolChunk : chunk.toolCallChunks_)
            blocks.push_back(toolCallChunkToJson(toolChunk));
    }
    return blocks;
}

[[nodiscard]] nlohmann::json metadataForUsage(const UsageMetadata& usage)
{
    nlohmann::json metadata = nlohmann::json::object();
    if (!usage.empty())
        metadata["usage"] = usageMetadataToJson(usage);
    return metadata;
}

} // namespace

Result<BaseMessage> BaseChatModel::stream(
    const std::vector<BaseMessage>& messages,
    AIMessageChunkHandler onChunk)
{
    auto response = invoke(messages);
    if (!response.isOk())
        return response.status();
    if (onChunk) {
        if (!response->content_.empty()) {
            if (auto status = onChunk(AIMessageChunk {
                    .text_ = response->content_,
                    .contentBlocks_ = contentBlocksFromText(response->content_),
                });
                !status.isOk()) {
                return status;
            }
        }
        if (auto status = onChunk(AIMessageChunk {
                .message_ = *response,
                .usageMetadata_ = response->usageMetadata_,
                .metadata_ = metadataForUsage(response->usageMetadata_),
                .done_ = true,
            });
            !status.isOk()) {
            return status;
        }
    }
    return response;
}

Result<std::vector<BaseMessage>> BaseChatModel::batch(
    const std::vector<std::vector<BaseMessage>>& inputs)
{
    std::vector<BaseMessage> responses;
    responses.reserve(inputs.size());
    for (const auto& messages : inputs) {
        auto response = invoke(messages);
        if (!response.isOk())
            return response.status();
        responses.push_back(std::move(*response));
    }
    return responses;
}

Result<std::shared_ptr<BaseChatModel>> BaseChatModel::bindTools(
    ChatModelToolBinding binding) const
{
    (void)binding;
    return Status::unimplemented("chat model does not support bindTools");
}

FakeChatModel::FakeChatModel(std::vector<BaseMessage> responses)
    : responses_(std::move(responses))
{
}

void FakeChatModel::pushResponse(BaseMessage message)
{
    std::lock_guard lock(mutex_);
    responses_.push_back(std::move(message));
}

std::size_t FakeChatModel::calls() const noexcept
{
    std::lock_guard lock(mutex_);
    return calls_;
}

Result<BaseMessage> FakeChatModel::invoke(const std::vector<BaseMessage>& messages)
{
    (void)messages;

    std::lock_guard lock(mutex_);
    ++calls_;
    if (next_ >= responses_.size())
        return Status::outOfRange("fake chat model has no scripted response");
    return responses_[next_++];
}

NodeHandler makeModelNode(
    std::shared_ptr<BaseChatModel> model,
    ModelNodeOptions options)
{
    return [model = std::move(model), options = std::move(options)](
               const State& state,
               Runtime& context) -> Result<StateUpdate> {
        if (!model)
            return Status::invalidArgument("model node requires a chat model");

        auto messages = messagesFromStateJson(state.view(), options.messagesField_);
        if (!messages.isOk())
            return messages.status();

        Result<BaseMessage> response = options.stream_
            ? model->stream(
                *messages,
                [&context](const AIMessageChunk& chunk) -> Status {
                    auto contentBlocks = contentBlocksForChunk(chunk);
                    auto toolCallChunks = toolCallChunksToJson(chunk.toolCallChunks_);
                    if (chunk.text_.empty() && contentBlocks.empty() && toolCallChunks.empty())
                        return Status::ok();
                    auto event = RuntimeEvent::create(RuntimeEventType::Token);
                    event.payload_ = {
                        { "text", chunk.text_ },
                        { "content_blocks", std::move(contentBlocks) },
                        { "tool_call_chunks", std::move(toolCallChunks) },
                        { "metadata", chunk.metadata_ },
                    };
                    return context.streamWriter().publish(std::move(event));
                })
            : model->invoke(*messages);
        if (!response.isOk())
            return response.status();

        return StateUpdate::fromJsonValue({
            { options.messagesField_, nlohmann::json::array({ baseMessageToJson(*response) }) },
        });
    };
}

} // namespace lgc
