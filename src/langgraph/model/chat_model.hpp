#pragma once

#include "foundation/json/json_schema.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/graph/state_graph.hpp"
#include "langgraph/message/message.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

struct ToolCallChunk {
    std::optional<std::size_t> index_;
    std::string id_;
    std::string name_;
    std::string argumentsDelta_;
};

struct AIMessageChunk {
    std::string text_;
    nlohmann::json contentBlocks_ { nlohmann::json::array() };
    std::vector<ToolCallChunk> toolCallChunks_;
    std::optional<BaseMessage> message_;
    UsageMetadata usageMetadata_;
    nlohmann::json metadata_ { nlohmann::json::object() };
    bool done_ { false };
};

using AIMessageChunkHandler = std::function<Status(const AIMessageChunk&)>;

/// Optional tokenizer/counter interface for model adapters that need local usage accounting when a
/// provider does not return usage metadata.
class ITokenCounter {
public:
    virtual ~ITokenCounter() = default;

    [[nodiscard]] virtual Result<std::uint64_t> countTextTokens(std::string_view text) = 0;
    [[nodiscard]] virtual Result<std::uint64_t> countMessageTokens(
        const std::vector<BaseMessage>& messages)
        = 0;
};

struct ChatModelTool {
    std::string name_;
    std::string description_;
    JsonSchema inputSchema_ { JsonSchema::object() };
};

struct ChatModelToolBinding {
    std::vector<ChatModelTool> tools_;
    /// Mirrors LangChain's tool_choice parameter. Null leaves provider default behavior.
    nlohmann::json toolChoice_ = nullptr;
    std::optional<bool> parallelToolCalls_;
};

/// Minimal LangChain-style chat model interface. Provider adapters implement this contract.
class BaseChatModel {
public:
    virtual ~BaseChatModel() = default;

    [[nodiscard]] virtual Result<BaseMessage> invoke(const std::vector<BaseMessage>& messages) = 0;
    [[nodiscard]] virtual Result<BaseMessage> stream(
        const std::vector<BaseMessage>& messages,
        AIMessageChunkHandler onChunk);
    [[nodiscard]] virtual Result<std::vector<BaseMessage>> batch(
        const std::vector<std::vector<BaseMessage>>& inputs);
    [[nodiscard]] virtual Result<std::shared_ptr<BaseChatModel>> bindTools(
        ChatModelToolBinding binding) const;
};

/// Deterministic in-memory model for tests and examples.
class FakeChatModel final : public BaseChatModel {
public:
    FakeChatModel() = default;
    explicit FakeChatModel(std::vector<BaseMessage> responses);

    void pushResponse(BaseMessage message);
    [[nodiscard]] std::size_t calls() const noexcept;

    [[nodiscard]] Result<BaseMessage> invoke(const std::vector<BaseMessage>& messages) override;

private:
    mutable std::mutex mutex_;
    std::vector<BaseMessage> responses_;
    std::size_t next_ { 0 };
    std::size_t calls_ { 0 };
};

struct ModelNodeOptions {
    /// State field containing serialized BaseMessage values.
    std::string messagesField_ { "messages" };
    /// When true, invoke BaseChatModel::stream and emit Token events for chunks.
    bool stream_ { false };
};

/// Create a graph node that invokes a chat model and appends its assistant message.
[[nodiscard]] NodeHandler makeModelNode(
    std::shared_ptr<BaseChatModel> model,
    ModelNodeOptions options = {});

} // namespace lc
