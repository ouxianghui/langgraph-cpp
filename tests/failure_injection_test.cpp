#include "foundation/event/i_event_sink.hpp"
#include "foundation/executor/i_executor.hpp"
#include "foundation/network/i_http_client.hpp"
#include "langgraph/checkpoint/checkpointer.hpp"
#include "langgraph/graph/state_graph.hpp"
#include "langgraph/model/provider_chat_model.hpp"
#include "langgraph/store/store.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class InjectedCheckpointSaver final : public lgc::BaseCheckpointSaver {
public:
    enum class FailurePoint {
        None,
        Put,
        PutWrites,
        GetTuple,
    };

    explicit InjectedCheckpointSaver(FailurePoint point)
        : point_(point)
    {
    }

    [[nodiscard]] lgc::Result<void> put(lgc::Checkpoint checkpoint) override
    {
        if (point_ == FailurePoint::Put)
            return lgc::Status::unavailable("injected checkpoint put failure");
        return inner_.put(std::move(checkpoint));
    }

    [[nodiscard]] lgc::Result<void> putWrites(lgc::CheckpointWriteSet writes) override
    {
        if (point_ == FailurePoint::PutWrites)
            return lgc::Status::unavailable("injected checkpoint put_writes failure");
        return inner_.putWrites(std::move(writes));
    }

    [[nodiscard]] lgc::Result<std::optional<lgc::Checkpoint>> get(lgc::CheckpointQuery query) override
    {
        return inner_.get(std::move(query));
    }

    [[nodiscard]] lgc::Result<std::optional<lgc::CheckpointTuple>> getTuple(
        lgc::CheckpointQuery query) override
    {
        if (point_ == FailurePoint::GetTuple)
            return lgc::Status::unavailable("injected checkpoint get_tuple failure");
        return inner_.getTuple(std::move(query));
    }

    [[nodiscard]] lgc::Result<std::vector<lgc::CheckpointTuple>> list(
        lgc::CheckpointListOptions options) override
    {
        return inner_.list(std::move(options));
    }

    [[nodiscard]] lgc::Result<void> deleteThread(std::string_view threadId) override
    {
        return inner_.deleteThread(threadId);
    }

    [[nodiscard]] lgc::Result<lgc::CheckpointMaintenanceResult> prune(
        std::string_view threadId,
        const lgc::CheckpointPruneOptions& options) override
    {
        return inner_.prune(threadId, options);
    }

    [[nodiscard]] lgc::Result<lgc::CheckpointMaintenanceResult> copyThread(
        lgc::CheckpointCopyThreadOptions options) override
    {
        return inner_.copyThread(std::move(options));
    }

    [[nodiscard]] lgc::Result<lgc::CheckpointMaintenanceResult> deleteForRuns(
        lgc::CheckpointDeleteForRunsOptions options) override
    {
        return inner_.deleteForRuns(std::move(options));
    }

    [[nodiscard]] lgc::Result<lgc::DeltaChannelHistories> getDeltaChannelHistory(
        lgc::DeltaChannelHistoryQuery query) override
    {
        return inner_.getDeltaChannelHistory(std::move(query));
    }

private:
    FailurePoint point_ { FailurePoint::None };
    lgc::InMemorySaver inner_;
};

class RejectingEventSink final : public lgc::IEventSink {
public:
    [[nodiscard]] lgc::Status publish(lgc::RuntimeEvent) override
    {
        return lgc::Status::permissionDenied("injected event sink failure");
    }

    [[nodiscard]] lgc::Status flush() override { return lgc::Status::ok(); }
    [[nodiscard]] lgc::Status waitIdle(Duration) override { return lgc::Status::ok(); }
    [[nodiscard]] lgc::Status close(Duration) override
    {
        closed_ = true;
        return lgc::Status::ok();
    }
    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }

private:
    bool closed_ { false };
};

class RejectingStore final : public lgc::BaseStore {
public:
    [[nodiscard]] lgc::Result<std::vector<lgc::StoreBatchResult>> batch(
        std::vector<lgc::StoreOp>) override
    {
        return lgc::Status::unavailable("injected store batch failure");
    }
};

class RejectingExecutor final : public lgc::IExecutor {
public:
    [[nodiscard]] lgc::Status post(Task, std::source_location = std::source_location::current()) override
    {
        return lgc::Status::unavailable("injected executor post failure");
    }

    [[nodiscard]] lgc::Status postDelayed(
        Duration,
        Task,
        std::source_location = std::source_location::current()) override
    {
        return lgc::Status::unavailable("injected executor postDelayed failure");
    }

    [[nodiscard]] lgc::Status execute(
        Task,
        std::source_location = std::source_location::current()) override
    {
        return lgc::Status::unavailable("injected executor execute failure");
    }

    [[nodiscard]] lgc::Status executeAndWait(
        Task,
        std::source_location = std::source_location::current()) override
    {
        return lgc::Status::unavailable("injected executor executeAndWait failure");
    }

    [[nodiscard]] lgc::Status waitIdle(Duration) override { return lgc::Status::ok(); }
    [[nodiscard]] lgc::Status close(Duration) override
    {
        closed_ = true;
        return lgc::Status::ok();
    }
    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }
    [[nodiscard]] bool isExecutorThread() const noexcept override { return false; }

private:
    bool closed_ { false };
};

class RejectingHttpClient final : public lgc::IHttpClient {
public:
    [[nodiscard]] lgc::HttpResult send(lgc::HttpRequest, lgc::HttpRequestOptions) override
    {
        return lgc::Status::unavailable("injected http transport failure");
    }

    [[nodiscard]] lgc::Status sendAsync(
        lgc::HttpRequest,
        lgc::HttpRequestOptions,
        lgc::HttpCallback callback) override
    {
        callback(lgc::Status::unavailable("injected async http transport failure"));
        return lgc::Status::unavailable("injected async http transport failure");
    }

    [[nodiscard]] lgc::HttpResult sendStreaming(
        lgc::HttpRequest,
        lgc::HttpRequestOptions,
        lgc::HttpBodyChunkCallback,
        lgc::HttpStreamOptions = {}) override
    {
        return lgc::Status::unavailable("injected streaming http transport failure");
    }

    [[nodiscard]] lgc::HttpResult sendSse(
        lgc::HttpRequest,
        lgc::HttpRequestOptions,
        lgc::ServerSentEventCallback,
        lgc::HttpStreamOptions = {}) override
    {
        return lgc::Status::unavailable("injected sse http transport failure");
    }

    [[nodiscard]] std::shared_ptr<lgc::IAuthorizationProvider> authorizationProvider() const override
    {
        return nullptr;
    }

    [[nodiscard]] lgc::Status close() override
    {
        closed_ = true;
        return lgc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }

private:
    bool closed_ { false };
};

[[nodiscard]] lgc::CompiledStateGraph makeSingleNodeGraph()
{
    lgc::StateGraph graph;
    assert(graph.addNode("work", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"ok":true})");
    }).isOk());
    assert(graph.setEntryPoint("work").isOk());
    assert(graph.setFinishPoint("work").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

[[nodiscard]] lgc::CompiledStateGraph makeParallelGraph()
{
    lgc::StateGraph graph;
    assert(graph.addNode("start", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::empty();
    }).isOk());
    assert(graph.addNode("left", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"items":["left"]})");
    }).isOk());
    assert(graph.addNode("right", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"items":["right"]})");
    }).isOk());
    assert(graph.addNode("join", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"joined":true})");
    }).isOk());
    assert(graph.setEntryPoint("start").isOk());
    assert(graph.addEdge("start", "left").isOk());
    assert(graph.addEdge("start", "right").isOk());
    assert(graph.addEdge(std::vector<lgc::NodeId> { "left", "right" }, "join").isOk());
    assert(graph.setFinishPoint("join").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

void testCheckpointPutFailurePropagates()
{
    auto graph = makeSingleNodeGraph();
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());

    lgc::RunOptions options;
    options.threadId_ = "failure-checkpoint-put";
    options.checkpointer_ = std::make_shared<InjectedCheckpointSaver>(
        InjectedCheckpointSaver::FailurePoint::Put);

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lgc::StatusCode::Unavailable);
}

void testCheckpointPutWritesFailurePropagates()
{
    auto graph = makeParallelGraph();
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());

    lgc::RunOptions options;
    options.threadId_ = "failure-checkpoint-put-writes";
    options.checkpointer_ = std::make_shared<InjectedCheckpointSaver>(
        InjectedCheckpointSaver::FailurePoint::PutWrites);
    options.durability_ = lgc::Durability::Sync;
    options.reducers_.set("items", lgc::ReducerKind::Append);
    options.maxConcurrency_ = 2;

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lgc::StatusCode::Unavailable);
}

void testCheckpointGetTupleFailurePropagates()
{
    auto graph = makeSingleNodeGraph();

    lgc::RunOptions options;
    options.checkpointer_ = std::make_shared<InjectedCheckpointSaver>(
        InjectedCheckpointSaver::FailurePoint::GetTuple);

    auto result = graph.resume("failure-resume", options);
    assert(!result.isOk());
    assert(result.status().code() == lgc::StatusCode::Unavailable);
}

void testEventSinkFailurePropagates()
{
    auto graph = makeSingleNodeGraph();
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());

    lgc::RunOptions options;
    options.eventSink_ = std::make_shared<RejectingEventSink>();

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lgc::StatusCode::PermissionDenied);
}

void testStoreFailurePropagatesFromNode()
{
    lgc::StateGraph graph;
    assert(graph.addNode("store", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
        auto store = context.store();
        if (!store)
            return lgc::Status::failedPrecondition("store is missing");
        auto stored = store->put({ "profile" }, "name", "edge");
        if (!stored.isOk())
            return stored.status();
        return lgc::StateUpdate::empty();
    }).isOk());
    assert(graph.setEntryPoint("store").isOk());
    assert(graph.setFinishPoint("store").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());

    lgc::RunOptions options;
    options.store_ = std::make_shared<RejectingStore>();

    auto result = compiled->invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lgc::StatusCode::Unavailable);
}

void testExecutorFailurePropagatesFromParallelDispatch()
{
    auto graph = makeParallelGraph();
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());

    lgc::RunOptions options;
    options.executor_ = std::make_shared<RejectingExecutor>();
    options.maxConcurrency_ = 2;
    options.reducers_.set("items", lgc::ReducerKind::Append);

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lgc::StatusCode::Unavailable);
}

void testHttpTransportFailurePropagatesFromProviderModel()
{
    auto http = std::make_shared<RejectingHttpClient>();
    lgc::ProviderChatModel model(
        lgc::ProviderChatModelOptions::openAICompatible(http, "failure-model", "token"));

    auto response = model.invoke({ lgc::BaseMessage::human("hello") });
    assert(!response.isOk());
    assert(response.status().code() == lgc::StatusCode::Unavailable);
}

} // namespace

int main()
{
    testCheckpointPutFailurePropagates();
    testCheckpointPutWritesFailurePropagates();
    testCheckpointGetTupleFailurePropagates();
    testEventSinkFailurePropagates();
    testStoreFailurePropagatesFromNode();
    testExecutorFailurePropagatesFromParallelDispatch();
    testHttpTransportFailurePropagatesFromProviderModel();
    return 0;
}
