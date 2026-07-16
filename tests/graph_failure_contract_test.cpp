#include <langgraph_cpp/langgraph.hpp>

#include "foundation/event/i_event_sink.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

lgc::State stateFromJson(const char* text)
{
    auto state = lgc::State::fromJson(text);
    assert(state.isOk());
    return *state;
}

class RejectingEventSink final : public lgc::IEventSink {
public:
    using Duration = lgc::IEventSink::Duration;

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

class FailingPutSaver final : public lgc::BaseCheckpointSaver {
public:
    [[nodiscard]] lgc::Result<void> put(lgc::Checkpoint) override
    {
        return lgc::Status::unavailable("injected put failure");
    }
    [[nodiscard]] lgc::Result<void> putWrites(lgc::CheckpointWriteSet writes) override
    {
        return inner_.putWrites(std::move(writes));
    }
    [[nodiscard]] lgc::Result<std::optional<lgc::Checkpoint>> get(lgc::CheckpointQuery query) override
    {
        return inner_.get(std::move(query));
    }
    [[nodiscard]] lgc::Result<std::optional<lgc::CheckpointTuple>> getTuple(
        lgc::CheckpointQuery query) override
    {
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
    lgc::InMemorySaver inner_;
};

[[nodiscard]] bool hasEventType(const lgc::RunResult& result, lgc::RuntimeEventType type)
{
    for (const auto& event : result.events_) {
        if (event.type_ == type)
            return true;
    }
    return false;
}

[[nodiscard]] lgc::CompiledStateGraph makeBoomGraph()
{
    lgc::StateGraph graph;
    assert(graph.addNode("boom", [](const lgc::State&, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        return lgc::Status::failedPrecondition("node boom");
    }).isOk());
    assert(graph.addEdge(std::string(lgc::START), "boom").isOk());
    assert(graph.addEdge("boom", std::string(lgc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

[[nodiscard]] lgc::CompiledStateGraph makeOkGraph()
{
    lgc::StateGraph graph;
    assert(graph.addNode("ok", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"ok":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lgc::START), "ok").isOk());
    assert(graph.addEdge("ok", std::string(lgc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

void testNodeFailureReturnsOkRunResultWithEvents()
{
    auto graph = makeBoomGraph();
    lgc::RunOptions options;
    options.collectEvents_ = true;

    auto result = graph.invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->status_ == lgc::RunStatus::Failed);
    assert(result->state_.view().at("__run_error__").at("code") == "failed_precondition");
    assert(hasEventType(*result, lgc::RuntimeEventType::RunStarted));
    assert(hasEventType(*result, lgc::RuntimeEventType::NodeFailed)
        || hasEventType(*result, lgc::RuntimeEventType::RunFailed));
}

void testCancellationReturnsOkCancelledRunResult()
{
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;

    lgc::StateGraph graph;
    assert(graph.addNode("slow", [&](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        ready.notify_all();
        while (!context.cancellationToken().cancelled())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return context.cancellationToken().check("cancelled");
    }).isOk());
    assert(graph.addEdge(std::string(lgc::START), "slow").isOk());
    assert(graph.addEdge("slow", std::string(lgc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    lgc::CancellationSource source;
    lgc::RunOptions options;
    options.cancellationToken_ = source.token();
    options.collectEvents_ = true;

    auto future = std::async(std::launch::async, [&] {
        return compiled->invoke(stateFromJson("{}"), options);
    });
    {
        std::unique_lock lock(mutex);
        assert(ready.wait_for(lock, std::chrono::seconds(2), [&] { return started; }));
    }
    assert(source.cancel("cancel contract test"));
    auto result = future.get();
    assert(result.isOk());
    assert(result->status_ == lgc::RunStatus::Cancelled);
    assert(result->state_.view().at("__run_error__").at("status") == "cancelled");
    assert(hasEventType(*result, lgc::RuntimeEventType::RunStarted));
}

void testMaxStepsReturnsOkMaxStepsExceeded()
{
    lgc::StateGraph graph;
    assert(graph.addNode("tick", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"n":1})");
    }).isOk());
    assert(graph.addEdge(std::string(lgc::START), "tick").isOk());
    assert(graph.addConditionalEdges(
        "tick",
        [](const lgc::State&, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            return std::string("tick");
        }).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    lgc::RunOptions options;
    options.limits_ = lgc::ResourceLimits {}.maxSteps(2);
    options.collectEvents_ = true;
    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->status_ == lgc::RunStatus::MaxStepsExceeded);
    assert(result->state_.view().at("__run_error__").at("status") == "max_steps_exceeded");
    assert(hasEventType(*result, lgc::RuntimeEventType::RunFailed)
        || hasEventType(*result, lgc::RuntimeEventType::RunStarted));
}

void testSubgraphFailurePropagatesAsParentNodeFailure()
{
    lgc::StateGraph child;
    assert(child.addNode("boom", [](const lgc::State&, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        return lgc::Status::unavailable("child unavailable");
    }).isOk());
    assert(child.addEdge(std::string(lgc::START), "boom").isOk());
    assert(child.addEdge("boom", std::string(lgc::END)).isOk());
    auto compiledChild = child.compile();
    assert(compiledChild.isOk());

    std::atomic<bool> afterRan { false };
    lgc::StateGraph parent;
    assert(parent.addSubgraph("sub", std::make_shared<lgc::CompiledStateGraph>(*compiledChild)).isOk());
    assert(parent.addNode("after", [&](const lgc::State&, lgc::Runtime&) {
        afterRan.store(true, std::memory_order_release);
        return lgc::StateUpdate::fromJson(R"({"after":true})");
    }).isOk());
    assert(parent.addEdge(std::string(lgc::START), "sub").isOk());
    assert(parent.addEdge("sub", "after").isOk());
    assert(parent.addEdge("after", std::string(lgc::END)).isOk());
    auto compiledParent = parent.compile();
    assert(compiledParent.isOk());

    lgc::RunOptions options;
    options.collectEvents_ = true;
    auto result = compiledParent->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->status_ == lgc::RunStatus::Failed);
    assert(result->state_.view().at("__run_error__").at("code") == "unavailable");
    assert(!afterRan.load(std::memory_order_acquire));
    assert(hasEventType(*result, lgc::RuntimeEventType::RunStarted));
}

void testStreamFailureResultUsesRunStatusNotResultError()
{
    auto graph = makeBoomGraph();
    auto stream = graph.streamProjected(stateFromJson("{}"));
    assert(stream.isOk());
    while (true) {
        auto part = stream->next();
        assert(part.isOk());
        if (!part->has_value())
            break;
    }
    auto result = stream->result();
    assert(result.isOk());
    assert(result->status_ == lgc::RunStatus::Failed);
    assert(result->state_.view().at("__run_error__").at("code") == "failed_precondition");
}

void testResumeCommandValidationAfterStartReturnsOkFailed()
{
    lgc::StateGraph graph;
    assert(graph.addNode("left", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::NodeOutput> {
        if (!context.hasResumeValue())
            return lgc::NodeOutput::interrupt(lgc::Interrupt { .id_ = "left-int", .value_ = { { "side", "L" } } });
        auto update = lgc::StateUpdate::fromJson(R"({"left":true})");
        if (!update.isOk())
            return update.status();
        return lgc::NodeOutput::update(std::move(*update));
    }).isOk());
    assert(graph.addNode("right", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::NodeOutput> {
        if (!context.hasResumeValue())
            return lgc::NodeOutput::interrupt(lgc::Interrupt { .id_ = "right-int", .value_ = { { "side", "R" } } });
        auto update = lgc::StateUpdate::fromJson(R"({"right":true})");
        if (!update.isOk())
            return update.status();
        return lgc::NodeOutput::update(std::move(*update));
    }).isOk());
    assert(graph.addEdge(std::string(lgc::START), "left").isOk());
    assert(graph.addEdge(std::string(lgc::START), "right").isOk());
    assert(graph.addEdge("left", std::string(lgc::END)).isOk());
    assert(graph.addEdge("right", std::string(lgc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lgc::InMemorySaver>();
    lgc::RunOptions options;
    options.threadId_ = "failure-contract-multi-interrupt";
    options.checkpointer_ = checkpointer;
    options.collectEvents_ = true;

    auto paused = compiled->invoke(stateFromJson("{}"), options);
    assert(paused.isOk());
    assert(paused->status_ == lgc::RunStatus::Paused);

    lgc::RunOptions badResume = options;
    badResume.command_ = lgc::Command::resume({ { "left-int", "only-left" } });
    auto failed = compiled->resume("failure-contract-multi-interrupt", badResume);
    assert(failed.isOk());
    assert(failed->status_ == lgc::RunStatus::Failed);
    assert(failed->state_.view().at("__run_error__").at("code") == "failed_precondition");
    assert(hasEventType(*failed, lgc::RuntimeEventType::RunStarted));
    assert(!failed->events_.empty());
}

void testEventSinkFailureKeepsCollectedEvents()
{
    auto graph = makeOkGraph();
    lgc::RunOptions options;
    options.eventSink_ = std::make_shared<RejectingEventSink>();
    options.collectEvents_ = true;

    auto result = graph.invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->status_ == lgc::RunStatus::Failed);
    assert(!result->events_.empty());
}

void testPreRunSetupFailuresStillReturnResultError()
{
    auto graph = makeBoomGraph();
    lgc::RunOptions options;
    auto missing = graph.resume("missing-thread", options);
    assert(!missing.isOk());
    assert(missing.status().code() == lgc::StatusCode::InvalidArgument);

    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();
    auto notFound = graph.resume("missing-thread", options);
    assert(!notFound.isOk());
    assert(notFound.status().code() == lgc::StatusCode::NotFound);
}

void testCheckpointPutFailureStillReturnsResultError()
{
    auto compiled = makeOkGraph();
    lgc::RunOptions options;
    options.threadId_ = "failure-contract-put";
    options.checkpointer_ = std::make_shared<FailingPutSaver>();
    auto result = compiled.invoke(stateFromJson("{}"), options);
    assert(!result.isOk());
    assert(result.status().code() == lgc::StatusCode::Unavailable);
}

} // namespace

int main()
{
    testNodeFailureReturnsOkRunResultWithEvents();
    testCancellationReturnsOkCancelledRunResult();
    testMaxStepsReturnsOkMaxStepsExceeded();
    testSubgraphFailurePropagatesAsParentNodeFailure();
    testStreamFailureResultUsesRunStatusNotResultError();
    testResumeCommandValidationAfterStartReturnsOkFailed();
    testEventSinkFailureKeepsCollectedEvents();
    testPreRunSetupFailuresStillReturnResultError();
    testCheckpointPutFailureStillReturnsResultError();
    return 0;
}
