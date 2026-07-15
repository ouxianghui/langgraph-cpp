#include "langgraph/model/llamacpp_chat_model.hpp"

#include "langgraph/tool/tool_call_grammar.hpp"

#include <array>
#include <limits>
#include <string_view>
#include <utility>

#include <llama.h>

namespace lgc {
namespace {

class LlamaBackend final {
public:
    LlamaBackend()
    {
        llama_backend_init();
    }

    ~LlamaBackend()
    {
        llama_backend_free();
    }
};

[[nodiscard]] LlamaBackend& llamaBackend()
{
    static LlamaBackend backend;
    return backend;
}

[[nodiscard]] std::string roleName(MessageType role)
{
    switch (role) {
    case MessageType::System:
        return "system";
    case MessageType::Human:
        return "user";
    case MessageType::AI:
        return "assistant";
    case MessageType::Tool:
        return "tool";
    }
    return "user";
}

[[nodiscard]] std::string manualPrompt(const std::vector<BaseMessage>& messages)
{
    std::string prompt;
    for (const auto& message : messages) {
        switch (message.type_) {
        case MessageType::System:
            prompt += "System: ";
            break;
        case MessageType::Human:
            prompt += "User: ";
            break;
        case MessageType::AI:
            prompt += "Assistant: ";
            break;
        case MessageType::Tool:
            prompt += "Tool ";
            prompt += message.name_;
            prompt += ": ";
            break;
        }
        prompt += message.content_;
        prompt += "\n";
    }
    prompt += "Assistant:";
    return prompt;
}

[[nodiscard]] std::string chatPrompt(const std::vector<BaseMessage>& messages)
{
    std::vector<std::string> roles;
    std::vector<std::string> contents;
    std::vector<llama_chat_message> chat;
    roles.reserve(messages.size());
    contents.reserve(messages.size());
    chat.reserve(messages.size());

    for (const auto& message : messages) {
        roles.push_back(roleName(message.type_));
        contents.push_back(message.content_);
    }
    for (std::size_t i = 0; i < messages.size(); ++i) {
        chat.push_back(llama_chat_message {
            .role = roles[i].c_str(),
            .content = contents[i].c_str(),
        });
    }

    const auto required = llama_chat_apply_template(
        nullptr,
        chat.data(),
        chat.size(),
        true,
        nullptr,
        0);
    if (required <= 0)
        return manualPrompt(messages);

    std::string prompt(static_cast<std::size_t>(required), '\0');
    const auto written = llama_chat_apply_template(
        nullptr,
        chat.data(),
        chat.size(),
        true,
        prompt.data(),
        required);
    if (written <= 0)
        return manualPrompt(messages);

    prompt.resize(static_cast<std::size_t>(written));
    return prompt;
}

[[nodiscard]] Result<std::vector<llama_token>> tokenizeText(
    const llama_vocab* vocab,
    std::string_view text,
    std::uint32_t contextSize,
    bool addSpecial,
    std::string_view emptyMessage)
{
    if (!vocab)
        return Status::failedPrecondition("llama.cpp vocab is not loaded");
    if (text.empty())
        return Status::invalidArgument(std::string(emptyMessage));
    if (contextSize == 0)
        return Status::invalidArgument("llama.cpp context size must be greater than zero");
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        return Status::invalidArgument("llama.cpp text is too large");

    std::vector<llama_token> tokens(contextSize);
    const auto tokenCount = llama_tokenize(
        vocab,
        text.data(),
        static_cast<int32_t>(text.size()),
        tokens.data(),
        static_cast<int32_t>(tokens.size()),
        addSpecial,
        true);
    if (tokenCount < 0)
        return Status::resourceExhausted("llama.cpp text exceeds context size");
    if (tokenCount == 0)
        return Status::invalidArgument("llama.cpp text produced no tokens");

    tokens.resize(static_cast<std::size_t>(tokenCount));
    return tokens;
}

[[nodiscard]] Result<std::vector<llama_token>> tokenizePrompt(
    const llama_vocab* vocab,
    std::string_view prompt,
    std::uint32_t contextSize)
{
    return tokenizeText(vocab, prompt, contextSize, true, "llama.cpp prompt cannot be empty");
}

[[nodiscard]] Result<std::string> tokenToPiece(const llama_vocab* vocab, llama_token token)
{
    std::array<char, 256> stackBuffer {};
    auto size = llama_token_to_piece(
        vocab,
        token,
        stackBuffer.data(),
        static_cast<int32_t>(stackBuffer.size()),
        0,
        true);
    if (size < 0) {
        std::string buffer(static_cast<std::size_t>(-size), '\0');
        size = llama_token_to_piece(
            vocab,
            token,
            buffer.data(),
            static_cast<int32_t>(buffer.size()),
            0,
            true);
        if (size < 0)
            return Status::internal("llama.cpp failed to convert token to text");
        buffer.resize(static_cast<std::size_t>(size));
        return buffer;
    }

    return std::string(stackBuffer.data(), static_cast<std::size_t>(size));
}

[[nodiscard]] UsageMetadata llamaUsageMetadata(
    const LlamaCppChatModelOptions& options,
    std::uint64_t inputTokens,
    std::uint64_t outputTokens)
{
    UsageMetadata usage;
    usage.source_ = UsageMetadataSource::Local;
    usage.provider_ = "llama.cpp";
    usage.model_ = options.modelPath_;
    usage.tokens_.inputTokens_ = inputTokens;
    usage.tokens_.outputTokens_ = outputTokens;
    usage.tokens_.totalTokens_ = inputTokens + outputTokens;
    return usage;
}

} // namespace

struct LlamaCppChatModel::Impl {
    llama_model* model_ { nullptr };
    const llama_vocab* vocab_ { nullptr };
    bool loaded_ { false };

    [[nodiscard]] Result<void> load(const LlamaCppChatModelOptions& options)
    {
        if (loaded_)
            return okResult();

        if (options.modelPath_.empty())
            return Status::invalidArgument("llama.cpp model path cannot be empty");
        if (options.contextSize_ == 0)
            return Status::invalidArgument("llama.cpp context size must be greater than zero");
        if (options.maxTokens_ == 0)
            return Status::invalidArgument("llama.cpp max tokens must be greater than zero");

        (void)llamaBackend();

        auto params = llama_model_default_params();
        model_ = llama_model_load_from_file(options.modelPath_.c_str(), params);
        if (!model_)
            return Status::notFound("failed to load llama.cpp GGUF model: " + options.modelPath_);

        vocab_ = llama_model_get_vocab(model_);
        if (!vocab_) {
            llama_model_free(model_);
            model_ = nullptr;
            return Status::failedPrecondition("llama.cpp model did not expose a vocab");
        }

        loaded_ = true;
        return okResult();
    }

    ~Impl()
    {
        if (model_)
            llama_model_free(model_);
    }
};

LlamaCppChatModel::LlamaCppChatModel(LlamaCppChatModelOptions options)
    : options_(std::move(options))
    , impl_(std::make_unique<Impl>())
{
}

LlamaCppChatModel::~LlamaCppChatModel() = default;

const LlamaCppChatModelOptions& LlamaCppChatModel::options() const noexcept
{
    return options_;
}

Result<void> LlamaCppChatModel::load()
{
    std::lock_guard lock(mutex_);
    return impl_->load(options_);
}

Result<std::uint64_t> LlamaCppChatModel::countTextTokens(std::string_view text)
{
    if (text.empty())
        return static_cast<std::uint64_t>(0);

    std::lock_guard lock(mutex_);
    auto loadResult = impl_->load(options_);
    if (!loadResult.isOk())
        return loadResult.status();

    auto tokens = tokenizeText(
        impl_->vocab_,
        text,
        options_.contextSize_,
        false,
        "llama.cpp text cannot be empty");
    if (!tokens.isOk())
        return tokens.status();
    return static_cast<std::uint64_t>(tokens->size());
}

Result<std::uint64_t> LlamaCppChatModel::countMessageTokens(
    const std::vector<BaseMessage>& messages)
{
    std::lock_guard lock(mutex_);
    auto loadResult = impl_->load(options_);
    if (!loadResult.isOk())
        return loadResult.status();

    auto prompt = chatPrompt(messages);
    auto tokens = tokenizePrompt(impl_->vocab_, prompt, options_.contextSize_);
    if (!tokens.isOk())
        return tokens.status();
    return static_cast<std::uint64_t>(tokens->size());
}

Result<BaseMessage> LlamaCppChatModel::invoke(const std::vector<BaseMessage>& messages)
{
    return stream(messages, {});
}

Result<BaseMessage> LlamaCppChatModel::stream(
    const std::vector<BaseMessage>& messages,
    AIMessageChunkHandler onChunk)
{
    std::lock_guard lock(mutex_);
    auto loadResult = impl_->load(options_);
    if (!loadResult.isOk())
        return loadResult.status();

    auto contextParams = llama_context_default_params();
    contextParams.n_ctx = options_.contextSize_;
    if (options_.threads_ > 0) {
        contextParams.n_threads = options_.threads_;
        contextParams.n_threads_batch = options_.threads_;
    }

    llama_context* context = llama_init_from_model(impl_->model_, contextParams);
    if (!context)
        return Status::internal("failed to create llama.cpp context");

    struct ContextGuard {
        llama_context* context_ { nullptr };
        ~ContextGuard()
        {
            if (context_)
                llama_free(context_);
        }
    } contextGuard { context };

    auto prompt = chatPrompt(messages);
    auto tokens = tokenizePrompt(impl_->vocab_, prompt, llama_n_ctx(context));
    if (!tokens.isOk())
        return tokens.status();

    auto promptBatch = llama_batch_get_one(tokens->data(), static_cast<int32_t>(tokens->size()));
    if (llama_decode(context, promptBatch) != 0)
        return Status::internal("llama.cpp failed to decode prompt");

    auto samplerParams = llama_sampler_chain_default_params();
    llama_sampler* sampler = llama_sampler_chain_init(samplerParams);
    if (!sampler)
        return Status::internal("failed to create llama.cpp sampler");

    struct SamplerGuard {
        llama_sampler* sampler_ { nullptr };
        ~SamplerGuard()
        {
            if (sampler_)
                llama_sampler_free(sampler_);
        }
    } samplerGuard { sampler };

    if (!options_.grammar_.empty()) {
        const auto root = options_.grammarRoot_.empty() ? "root" : options_.grammarRoot_.c_str();
        llama_sampler* grammar = llama_sampler_init_grammar(
            impl_->vocab_,
            options_.grammar_.c_str(),
            root);
        if (!grammar)
            return Status::invalidArgument("llama.cpp failed to parse GBNF grammar");
        llama_sampler_chain_add(sampler, grammar);
    }

    if (options_.temperature_ > 0.0F) {
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(options_.temperature_));
        const auto seed = options_.seed_ == 0 ? LLAMA_DEFAULT_SEED : options_.seed_;
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(seed));
    } else {
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    }

    std::string output;
    const auto inputTokens = static_cast<std::uint64_t>(tokens->size());
    auto consumedTokens = static_cast<std::uint32_t>(tokens->size());
    for (std::uint32_t i = 0; i < options_.maxTokens_; ++i) {
        if (consumedTokens + 1 >= llama_n_ctx(context))
            break;

        const llama_token token = llama_sampler_sample(sampler, context, -1);
        if (llama_vocab_is_eog(impl_->vocab_, token))
            break;

        auto piece = tokenToPiece(impl_->vocab_, token);
        if (!piece.isOk())
            return piece.status();
        output += *piece;
        if (onChunk && !piece->empty()) {
            if (auto status = onChunk(AIMessageChunk {
                    .text_ = *piece,
                    .metadata_ = {
                        { "provider", "llama.cpp" },
                        { "token_index", i },
                    },
                });
                !status.isOk()) {
                return status;
            }
        }

        llama_sampler_accept(sampler, token);
        llama_token next = token;
        auto tokenBatch = llama_batch_get_one(&next, 1);
        if (llama_decode(context, tokenBatch) != 0)
            return Status::internal("llama.cpp failed to decode generated token");
        ++consumedTokens;
    }

    const auto generatedTokens = static_cast<std::uint64_t>(
        consumedTokens - static_cast<std::uint32_t>(inputTokens));
    const auto usage = llamaUsageMetadata(options_, inputTokens, generatedTokens);

    Result<BaseMessage> message = options_.parseToolCallJson_
        ? assistantMessageFromToolCallJson(std::move(output))
        : Result<BaseMessage>(BaseMessage::ai(std::move(output)));
    if (!message.isOk())
        return message.status();
    message->usageMetadata_ = usage;

    if (onChunk) {
        if (auto status = onChunk(AIMessageChunk {
                .message_ = *message,
                .usageMetadata_ = usage,
                .metadata_ = {
                    { "provider", "llama.cpp" },
                    { "generated_tokens", generatedTokens },
                    { "usage", usageMetadataToJson(usage) },
                },
                .done_ = true,
            });
            !status.isOk()) {
            return status;
        }
    }

    return message;
}

} // namespace lgc
