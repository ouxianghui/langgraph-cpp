#pragma once

#include "foundation/network/i_http_client.hpp"
#include "langgraph/model/chat_model.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

enum class ChatProviderKind : std::uint8_t {
    OpenAICompatible,
    Anthropic,
    Qwen,
    DeepSeek,
};

[[nodiscard]] std::string_view chatProviderName(ChatProviderKind provider) noexcept;

struct ProviderPromptOptions {
    /// Zero means no message-count trim.
    std::size_t maxMessages_ { 0 };
    /// Zero means no byte-budget trim. The newest required message is never truncated.
    std::size_t maxBytes_ { 0 };
};

struct ProviderChatModelOptions {
    ChatProviderKind provider_ { ChatProviderKind::OpenAICompatible };
    std::string model_;
    std::string path_ { "/v1/chat/completions" };
    std::string apiKey_;
    std::map<std::string, std::string> headers_;
    ProviderPromptOptions prompt_;
    std::size_t maxOutputTokens_ { 0 };
    bool includeUsage_ { true };
    ChatModelToolBinding toolBinding_;
    nlohmann::json extraRequestFields_ { nlohmann::json::object() };
    HttpStreamOptions streamOptions_;
    std::shared_ptr<IHttpClient> httpClient_;

    [[nodiscard]] static ProviderChatModelOptions openAICompatible(
        std::shared_ptr<IHttpClient> httpClient,
        std::string model,
        std::string apiKey = {});
    [[nodiscard]] static ProviderChatModelOptions anthropic(
        std::shared_ptr<IHttpClient> httpClient,
        std::string model,
        std::string apiKey = {});
    [[nodiscard]] static ProviderChatModelOptions qwen(
        std::shared_ptr<IHttpClient> httpClient,
        std::string model,
        std::string apiKey = {});
    [[nodiscard]] static ProviderChatModelOptions deepSeek(
        std::shared_ptr<IHttpClient> httpClient,
        std::string model,
        std::string apiKey = {});
};

/// HTTP-backed provider adapter for edge deployments.
///
/// The adapter is transport-agnostic: callers inject an IHttpClient configured with
/// retry, rate limit, circuit breaker, TLS, proxy, and queue policy. Tests can use a
/// fake IHttpClient without hitting real providers.
class ProviderChatModel final : public BaseChatModel {
public:
    explicit ProviderChatModel(ProviderChatModelOptions options);

    [[nodiscard]] Result<BaseMessage> invoke(const std::vector<BaseMessage>& messages) override;
    [[nodiscard]] Result<BaseMessage> stream(
        const std::vector<BaseMessage>& messages,
        AIMessageChunkHandler onChunk) override;

    [[nodiscard]] Result<std::vector<BaseMessage>> batch(
        const std::vector<std::vector<BaseMessage>>& inputs) override;
    [[nodiscard]] Result<std::shared_ptr<BaseChatModel>> bindTools(
        ChatModelToolBinding binding) const override;

    [[nodiscard]] const ProviderChatModelOptions& options() const noexcept;

private:
    [[nodiscard]] Result<std::vector<BaseMessage>> prepareMessages(
        const std::vector<BaseMessage>& messages) const;
    [[nodiscard]] Result<HttpRequest> buildRequest(
        const std::vector<BaseMessage>& messages,
        bool stream) const;
    [[nodiscard]] Result<BaseMessage> parseResponse(const HttpResponse& response) const;

    ProviderChatModelOptions options_;
};

} // namespace lc
