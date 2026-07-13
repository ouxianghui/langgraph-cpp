#pragma once

#include "langgraph/model/chat_model.hpp"

#ifndef LANGGRAPH_CPP_WITH_LLAMA_CPP
#define LANGGRAPH_CPP_WITH_LLAMA_CPP 0
#endif

#if LANGGRAPH_CPP_WITH_LLAMA_CPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lc {

/// Options for the optional llama.cpp-backed chat model adapter.
struct LlamaCppChatModelOptions {
    /// Path to a GGUF model file.
    std::string modelPath_;
    /// Requested context length. llama.cpp may clamp this based on model/runtime support.
    std::uint32_t contextSize_ { 4096 };
    /// Sampling temperature. Values <= 0 use greedy sampling.
    float temperature_ { 0.8F };
    /// Maximum number of generated tokens per invoke().
    std::uint32_t maxTokens_ { 256 };
    /// Sampling seed. 0 uses llama.cpp's default random seed.
    std::uint32_t seed_ { 0 };
    /// Decode threads. 0 lets llama.cpp choose its default.
    std::int32_t threads_ { 0 };
    /// Optional llama.cpp GBNF grammar applied during sampling.
    std::string grammar_;
    /// Grammar root rule. llama.cpp convention uses "root".
    std::string grammarRoot_ { "root" };
    /// Parse generated content as {"tool_calls":[...]} and return structured tool calls.
    bool parseToolCallJson_ { false };
};

/// Chat model adapter backed by llama.cpp. Available only when LANGGRAPH_CPP_WITH_LLAMA_CPP=ON.
class LlamaCppChatModel final : public BaseChatModel {
public:
    explicit LlamaCppChatModel(LlamaCppChatModelOptions options);
    ~LlamaCppChatModel() override;

    LlamaCppChatModel(const LlamaCppChatModel&) = delete;
    LlamaCppChatModel& operator=(const LlamaCppChatModel&) = delete;

    [[nodiscard]] const LlamaCppChatModelOptions& options() const noexcept;
    [[nodiscard]] Result<void> load();
    [[nodiscard]] Result<BaseMessage> invoke(const std::vector<BaseMessage>& messages) override;
    [[nodiscard]] Result<BaseMessage> stream(
        const std::vector<BaseMessage>& messages,
        AIMessageChunkHandler onChunk) override;

private:
    struct Impl;

    LlamaCppChatModelOptions options_;
    std::unique_ptr<Impl> impl_;
    mutable std::mutex mutex_;
};

} // namespace lc

#endif // LANGGRAPH_CPP_WITH_LLAMA_CPP
