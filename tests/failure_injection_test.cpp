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

class InjectedCheckpointSaver final : public lc::BaseCheckpointSaver {
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

    [[nodiscard]] lc::Result<void> put(lc::Checkpoint checkpoint) override
    {
        if (point_ == FailurePoint::Put)
            return lc::Status::unavailable("injected checkpoint put failure");
        return inner_.put(std::move(checkpoint));
    }

    [[nodiscard]] lc::Result<void> putWrites(lc::CheckpointWriteSet writes) override
    {
        if (point_ == FailurePoint::PutWrites)
            return lc::Status::unavailable("injected checkpoint put_writes failure");
        return inner_.putWrites(std::move(writes));
    }

    [[nodiscard]] lc::Result<std::optional<lc::Checkpoint>> get(lc::CheckpointQuery query) override
    {
        return inner_.get(std::move(query));
    }

    [[nodiscard]] lc::Result<std::optional<lc::CheckpointTuple>> getTuple(
        lc::CheckpointQuery query) override
    {
        if (point_ == FailurePoint::GetTuple)
            return lc::Status::unavailable("injected checkpoint get_tuple failure");
        return inner_.getTuple(std::move(query));
    }

    [[nodiscard]] lc::Result<std::vector<lc::CheckpointTuple>> list(
        lc::CheckpointListOptions options) override
    {
        return inner_.list(std::move(options));
    }

    [[nodiscard]] lc::Result<void> deleteThread(std::string_view threadId) override
    {
        return inner_.deleteThread(threadId);
    }

    [[nodiscard]] lc::Result<lc::CheckpointMaintenanceResult> prune(
        std::string_view threadId,
        const lc::CheckpointPruneOptions& options) override
    {
        return inner_.prune(threadId, options);
    }

    [[nodiscard]] lc::Result<lc::CheckpointMaintenanceResult> copyThread(
        lc::CheckpointCopyThreadOptions options) override
    {
        return inner_.copyThread(std::move(options));
    }

    [[nodiscard]] lc::Result<lc::CheckpointMaintenanceResult> deleteForRuns(
        lc::CheckpointDeleteForRunsOptions options) override
    {
        return inner_.deleteForRuns(std::move(options));
    }

    [[nodiscard]] lc::Result<lc::DeltaChannelHistories> getDeltaChannelHistory(
        lc::DeltaChannelHistoryQuery query) override
    {
        return inner_.getDeltaChannelHistory(std::move(query));
    }

private:
    FailurePoint point_ { FailurePoint::None };
    lc::InMemorySaver inner_;
};

class RejectingEventSink final : public lc::IEventSink {
public:
    [[nodiscard]] lc::Status publish(lc::RuntimeEvent) override
    {
        return lc::Status::permissionDenied("injected event sink failure");
    }

    [[nodiscard]] lc::Status flush() override { return lc::Status::ok(); }
    [[nodiscard]] lc::Status waitIdle(Duration) override { return lc::Status::ok(); }
    [[nodiscard]] lc::Status close(Duration) override
    {
        closed_ = true;
        return lc::Status::ok();
    }
    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }

private:
    bool closed_ { false };
};

class RejectingStore final : public lc::BaseStore {
public:
    [[nodiscard]] lc::Result<std::vector<lc::StoreBatchResult>> batch(
        std::vector<lc::StoreOp>) override
    {
        return lc::Status::unavailable("injected store batch failure");
    }
};

class RejectingExecutor final : public lc::IExecutor {
public:
    [[nodiscard]] lc::Status post(Task, std::source_location = std::source_location::current()) override
    {
        return lc::Status::unavailable("injected executor post failure");
    }

    [[nodiscard]] lc::Status postDelayed(
        Duration,
        Task,
        std::source_location = std::source_location::current()) override
    {
        return lc::Status::unavailable("injected executor postDelayed failure");
    }

    [[nodiscard]] lc::Status execute(
        Task,
        std::source_location = std::source_location::current()) override
    {
        return lc::Status::unavailable("injected executor execute failure");
    }

    [[nodiscard]] lc::Status executeAndWait(
        Task,
        std::source_location = std::source_location::current()) override
    {
        return lc::Status::unavailable("injected executor executeAndWait failure");
    }

    [[nodiscard]] lc::Status waitIdle(Duration) override { return lc::Status::ok(); }
    [[nodiscard]] lc::Status close(Duration) override
    {
        closed_ = true;
        return lc::Status::ok();
    }
    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }
    [[nodiscard]] bool isExecutorThread() const noexcept override { return false; }

private:
    bool closed_ { false };
};

class RejectingHttpClient final : public lc::IHttpClient {
public:
    [[nodiscard]] lc::HttpResult send(lc::HttpRequest, lc::HttpRequestOptions) override
    {
        return lc::Status::unavailable("injected http transport failure");
    }

    [[nodiscard]] lc::Status sendAsync(
        lc::HttpRequest,
        lc::HttpRequestOptions,
        lc::HttpCallback callback) override
    {
        callback(lc::Status::unavailable("injected async http transport failure"));
        return lc::Status::unavailable("injected async http transport failure");
    }

    [[nodiscard]] lc::HttpResult sendStreaming(
        lc::HttpRequest,
        lc::HttpRequestOptions,
        lc::HttpBodyChunkCallback,
        lc::HttpStreamOptions = {}) override
    {
        return lc::Status::unavailable("injected streaming http transport failure");
    }

    [[nodiscard]] lc::HttpResult sendSse(
        lc::HttpRequest,
        lc::HttpRequestOptions,
        lc::ServerSentEventCallback,
        lc::HttpStreamOptions = {}) override
    {
        return lc::Status::unavailable("injected sse http transport failure");
    }

    [[nodiscard]] std::shared_ptr<lc::IAuthorizationProvider> authorizationProvider() const override
    {
        return nullptr;
    }

    [[nodiscard]] lc::Status close() override
    {
        closed_ = true;
        return lc::Status::ok();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return closed_; }

private:
    bool closed_ { false };
};

[[nodiscard]] lc::CompiledStateGraph makeSingleNodeGraph()
{
    lc::StateGraph graph;
    assert(graph.addNode("work", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"ok":true})");
    }).isOk());
    assert(graph.setEntryPoint("work").isOk());
    assert(graph.setFinishPoint("work").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

[[nodiscard]] lc::CompiledStateGraph makeParallelGraph()
{
    lc::StateGraph graph;
    assert(graph.addNode("start", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.addNode("left", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"items":["left"]})");
    }).isOk());
    assert(graph.addNode("right", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"items":["right"]})");
    }).isOk());
    assert(graph.addNode("join", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"joined":true})");
    }).isOk());
    assert(graph.setEntryPoint("start").isOk());
    assert(graph.addEdge("start", "left").isOk());
    assert(graph.addEdge("start", "right").isOk());
    assert(graph.addEdge(std::vector<lc::NodeId> { "left", "right" }, "join").isOk());
    assert(graph.setFinishPoint("join").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

void testCheckpointPutFailurePropagates()
{
    auto graph = makeSingleNodeGraph();
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());

    lc::RunOptions options;
    options.threadId_ = "failure-checkpoint-put";
    options.checkpointer_ = std::make_shared<InjectedCheckpointSaver>(
        InjectedCheckpointSaver::FailurePoint::Put);

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::Unavailable);
}

void testCheckpointPutWritesFailurePropagates()
{
    auto graph = makeParallelGraph();
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());

    lc::RunOptions options;
    options.threadId_ = "failure-checkpoint-put-writes";
    options.checkpointer_ = std::make_shared<InjectedCheckpointSaver>(
        InjectedCheckpointSaver::FailurePoint::PutWrites);
    options.durability_ = lc::Durability::Sync;
    options.reducers_.set("items", lc::ReducerKind::Append);
    options.maxConcurrency_ = 2;

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::Unavailable);
}

void testCheckpointGetTupleFailurePropagates()
{
    auto graph = makeSingleNodeGraph();

    lc::RunOptions options;
    options.checkpointer_ = std::make_shared<InjectedCheckpointSaver>(
        InjectedCheckpointSaver::FailurePoint::GetTuple);

    auto result = graph.resume("failure-resume", options);
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::Unavailable);
}

void testEventSinkFailurePropagates()
{
    auto graph = makeSingleNodeGraph();
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());

    lc::RunOptions options;
    options.eventSink_ = std::make_shared<RejectingEventSink>();

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::PermissionDenied);
}

void testStoreFailurePropagatesFromNode()
{
    lc::StateGraph graph;
    assert(graph.addNode("store", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto store = context.store();
        if (!store)
            return lc::Status::failedPrecondition("store is missing");
        auto stored = store->put({ "profile" }, "name", "edge");
        if (!stored.isOk())
            return stored.status();
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.setEntryPoint("store").isOk());
    assert(graph.setFinishPoint("store").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());

    lc::RunOptions options;
    options.store_ = std::make_shared<RejectingStore>();

    auto result = compiled->invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::Unavailable);
}

void testExecutorFailurePropagatesFromParallelDispatch()
{
    auto graph = makeParallelGraph();
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());

    lc::RunOptions options;
    options.executor_ = std::make_shared<RejectingExecutor>();
    options.maxConcurrency_ = 2;
    options.reducers_.set("items", lc::ReducerKind::Append);

    auto result = graph.invoke(*input, options);
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::Unavailable);
}

void testHttpTransportFailurePropagatesFromProviderModel()
{
    auto http = std::make_shared<RejectingHttpClient>();
    lc::ProviderChatModel model(
        lc::ProviderChatModelOptions::openAICompatible(http, "failure-model", "token"));

    auto response = model.invoke({ lc::BaseMessage::human("hello") });
    assert(!response.isOk());
    assert(response.status().code() == lc::StatusCode::Unavailable);
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
