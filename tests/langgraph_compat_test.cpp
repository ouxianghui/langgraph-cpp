#include "langgraph/graph/state_graph.hpp"
#include "langgraph/message/message.hpp"
#include "langgraph/model/chat_model.hpp"
#include "langgraph/state/state_update.hpp"
#include "langgraph/tool/tool.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] lc::State stateFromJson(std::string text)
{
    auto state = lc::State::fromJson(std::move(text));
    assert(state.isOk());
    return std::move(*state);
}

[[nodiscard]] lc::StateUpdate updateFromJson(std::string text)
{
    auto update = lc::StateUpdate::fromJson(std::move(text));
    assert(update.isOk());
    return std::move(*update);
}

[[nodiscard]] nlohmann::json snapshotShape(const lc::StateSnapshot& snapshot)
{
    auto config = [](std::string threadId, std::string checkpointId, std::string checkpointNamespace) {
        nlohmann::json configurable = nlohmann::json::object();
        if (!threadId.empty())
            configurable["thread_id"] = std::move(threadId);
        if (!checkpointId.empty())
            configurable["checkpoint_id"] = std::move(checkpointId);
        configurable["checkpoint_ns"] = std::move(checkpointNamespace);
        return nlohmann::json {
            { "configurable", std::move(configurable) },
        };
    };

    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& task : snapshot.tasks_) {
        tasks.push_back({
            { "id", task.taskId_ },
            { "name", task.nodeId_ },
            { "checkpoint_ns", task.checkpointNamespace_ },
            { "error", task.error_.has_value() ? nlohmann::json(*task.error_) : nlohmann::json(nullptr) },
            { "interrupts", task.interrupts_ },
        });
    }

    nlohmann::json writes = nlohmann::json::array();
    for (const auto& write : snapshot.writes_) {
        writes.push_back({
            { "node", write.nodeId_ },
            { "update", write.update_.view() },
            { "metadata", write.metadata_ },
        });
    }

    nlohmann::json pendingWrites = nlohmann::json::array();
    for (const auto& write : snapshot.pendingWrites_) {
        pendingWrites.push_back({
            { "node", write.nodeId_ },
            { "update", write.update_.view() },
            { "metadata", write.metadata_ },
        });
    }

    const auto createdAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        snapshot.createdAt_.time_since_epoch()).count();

    return {
        { "values", snapshot.values_.view() },
        { "next", snapshot.next_ },
        { "tasks", std::move(tasks) },
        { "writes", std::move(writes) },
        { "pending_writes", std::move(pendingWrites) },
        { "config", config(snapshot.threadId_, snapshot.checkpointId_, snapshot.checkpointNamespace_) },
        { "parent_config", snapshot.parentCheckpointId_.has_value()
                ? config(snapshot.threadId_, *snapshot.parentCheckpointId_, snapshot.checkpointNamespace_)
                : nlohmann::json(nullptr) },
        { "metadata", {
            { "step", snapshot.step_ },
            { "created_at_ms", createdAtMs },
        } },
    };
}

[[nodiscard]] lc::State stateFromMessages(std::vector<lc::BaseMessage> messages)
{
    auto state = lc::State::fromJsonValue({
        { "messages", lc::messagesToJson(messages) },
    });
    assert(state.isOk());
    return std::move(*state);
}

[[nodiscard]] lc::CompiledStateGraph buildCountingGraph()
{
    lc::StateGraph graph;
    assert(graph.addNode("tick", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        const auto count = state.view().value("count", 0);
        return lc::StateUpdate::fromJsonValue({ { "count", count + 1 } });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "tick").isOk());
    assert(graph.addConditionalEdges(
        "tick",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            return state.view().at("count").get<int>() < 3 ? lc::NodeId("tick") : lc::NodeId(lc::END);
        },
        { "tick", std::string(lc::END) }).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    return std::move(*compiled);
}

void testRunnableConfigMergePatchAndEventMetadata()
{
    auto base = lc::RunnableConfig::fromJson({
        { "tags", nlohmann::json::array({ "base" }) },
        { "metadata", {
            { "tenant", "tenant-a" },
        } },
        { "callbacks", nlohmann::json::array({ std::string("cb1") }) },
        { "recursion_limit", 12 },
        { "max_concurrency", 3 },
        { "run_name", "base-run" },
        { "run_id", "run-config" },
        { "configurable", {
            { "thread_id", "config-thread" },
            { "checkpoint_ns", "root" },
            { "custom", "base" },
        } },
    });
    assert(base.isOk());

    auto call = lc::RunnableConfig::fromJson({
        { "tags", nlohmann::json::array({ "call" }) },
        { "metadata", {
            { "tenant", "tenant-b" },
        } },
        { "configurable", {
            { "checkpoint_id", "cp-1" },
            { "custom", "call" },
        } },
        { "custom_top", "top" },
    });
    assert(call.isOk());

    auto merged = lc::mergeRunnableConfigs({ *base, *call });
    assert(merged.isOk());
    assert(merged->tags_ == std::vector<std::string>({ "base", "call" }));
    assert(merged->metadata_.at("tenant") == "tenant-b");
    assert(merged->metadata_.at("thread_id") == "config-thread");
    assert(merged->metadata_.at("checkpoint_id") == "cp-1");
    assert(merged->metadata_.at("checkpoint_ns") == "root");
    assert(!merged->metadata_.contains("custom"));
    assert(!merged->metadata_.contains("custom_top"));
    assert(merged->configurable_.at("custom") == "call");
    assert(merged->configurable_.at("custom_top") == "top");
    assert(merged->runName_ == "base-run");
    assert(merged->runId_ == "run-config");
    assert(merged->recursionLimit_.value() == 12);
    assert(merged->maxConcurrency_.value() == 3);

    auto patched = lc::patchRunnableConfig(*merged, {
        { "callbacks", nlohmann::json::array({ std::string("cb2") }) },
        { "metadata", {
            { "request_id", "req-1" },
        } },
        { "configurable", {
            { "checkpoint_ns", "patched-root" },
            { "patched", true },
        } },
        { "recursion_limit", 8 },
    });
    assert(patched.isOk());
    assert(patched->runName_.empty());
    assert(patched->runId_.empty());
    assert(patched->callbacks_.front() == "cb2");
    assert(patched->metadata_.at("request_id") == "req-1");
    assert(patched->metadata_.at("checkpoint_ns") == "root");
    assert(patched->configurable_.at("checkpoint_ns") == "patched-root");
    assert(!patched->metadata_.contains("patched"));
    assert(patched->recursionLimit_.value() == 8);

    auto applied = lc::applyRunnableConfig(lc::RunOptions {}, *merged);
    assert(applied.isOk());
    assert(applied->threadId_ == "config-thread");
    assert(applied->checkpointNamespace_ == "root");
    assert(applied->runId_ == "run-config");
    assert(applied->runName_ == "base-run");
    assert(applied->maxConcurrency_ == 3);
    assert(applied->limits_.maxSteps_.value() == 12);
    assert(applied->metadata_.at("tenant") == "tenant-b");
    assert(applied->configurable_.at("checkpoint_id") == "cp-1");

    lc::StateGraph graph;
    assert(graph.addNode("node", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"ok":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "node").isOk());
    assert(graph.addEdge("node", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto stream = compiled->streamProjected(
        stateFromJson("{}"),
        *applied,
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Events },
            .capacity_ = 16,
            .langGraphProtocol_ = true,
        });
    assert(stream.isOk());

    bool sawTaggedEnvelope = false;
    auto live = std::move(*stream);
    for (;;) {
        auto part = live.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        if ((*part)->mode_ != lc::StreamMode::Events)
            continue;
        const auto& data = (*part)->data_;
        if (data.at("event") == "on_chain_start") {
            sawTaggedEnvelope = true;
            assert(data.at("tags").is_array());
            assert(data.at("tags").at(0) == "base");
            assert(data.at("tags").at(1) == "call");
            assert(data.at("metadata").at("tenant") == "tenant-b");
            assert(data.at("metadata").at("thread_id") == "config-thread");
            assert(data.at("metadata").at("checkpoint_ns") == "root");
            assert(data.at("metadata").at("checkpoint_id") == "cp-1");
            assert(!data.at("metadata").contains("custom_top"));
        }
    }
    auto result = live.result();
    assert(result.isOk());
    assert(result->runId_ == "run-config");
    assert(result->threadId_ == "config-thread");
    assert(sawTaggedEnvelope);
}

void testSnapshotFieldsAndHistoryOrder()
{
    auto compiled = buildCountingGraph();
    auto checkpointer = std::make_shared<lc::InMemorySaver>();

    lc::RunOptions options;
    options.threadId_ = "compat-history";
    options.checkpointer_ = checkpointer;

    auto result = compiled.invoke(stateFromJson(R"({"count":0})"), options);
    assert(result.isOk());
    assert(result->state_.view().at("count") == 3);

    auto history = compiled.getStateHistory("compat-history", options);
    assert(history.isOk());
    assert(history->size() == 4);
    assert(history->front().step_ == 3);
    assert(history->front().values_.view().at("count") == 3);
    assert(history->front().next_.empty());
    assert(history->at(2).step_ == 1);
    assert(history->at(2).values_.view().at("count") == 1);
    assert(history->back().step_ == 0);
    assert(history->back().values_.view().at("count") == 0);

    auto shaped = snapshotShape(history->at(2));
    assert(shaped.contains("values"));
    assert(shaped.contains("next"));
    assert(shaped.contains("tasks"));
    assert(shaped.contains("writes"));
    assert(shaped.contains("pending_writes"));
    assert(shaped.contains("parent_config"));
    const auto& config = shaped.at("config").at("configurable");
    assert(config.at("thread_id") == "compat-history");
    assert(config.at("checkpoint_id").is_string());
    assert(config.at("checkpoint_ns") == "");
    assert(shaped.at("metadata").at("step") == 1);
    assert(shaped.at("metadata").at("created_at_ms").is_number_integer());
}

void testInterruptReplay()
{
    lc::StateGraph graph;
    assert(graph.addNode("approve", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto answer = context.interrupt("approval", { { "question", "continue?" } });
        if (!answer.isOk())
            return answer.status();
        return lc::StateUpdate::fromJsonValue({ { "approved", *answer } });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "approve").isOk());
    assert(graph.addEdge("approve", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "compat-interrupt";
    options.checkpointer_ = checkpointer;

    auto paused = compiled->invoke(stateFromJson("{}"), options);
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(paused->state_.view().at("__interrupt__").at("interrupts").front().at("id") == "approval");

    auto latest = compiled->getState("compat-interrupt", options);
    assert(latest.isOk());
    auto replayedPause = compiled->replay("compat-interrupt", latest->checkpointId_, options);
    assert(replayedPause.isOk());
    assert(replayedPause->status_ == lc::RunStatus::Paused);
    assert(replayedPause->state_.view().at("__interrupt__").at("interrupts").front().at("id") == "approval");

    lc::RunOptions resumeOptions = options;
    resumeOptions.command_ = lc::Command::resume({ { "approval", true } });
    auto resumed = compiled->resume("compat-interrupt", resumeOptions);
    assert(resumed.isOk());
    assert(resumed->status_ == lc::RunStatus::Completed);
    assert(resumed->state_.view().at("approved") == true);
    assert(!resumed->state_.view().contains("__interrupt__"));
}

void testInterruptNodeRerunAndReplayAfterCompletion()
{
    auto beforeInterruptRuns = std::make_shared<std::atomic<int>>(0);
    auto afterInterruptRuns = std::make_shared<std::atomic<int>>(0);

    lc::StateGraph graph;
    assert(graph.addNode(
        "ask",
        [beforeInterruptRuns, afterInterruptRuns](
            const lc::State&,
            lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
            beforeInterruptRuns->fetch_add(1, std::memory_order_relaxed);
            auto answer = context.interrupt("approval", { { "question", "continue?" } });
            if (!answer.isOk())
                return answer.status();
            afterInterruptRuns->fetch_add(1, std::memory_order_relaxed);
            return lc::StateUpdate::fromJsonValue({
                { "approved", *answer },
                { "before_runs", beforeInterruptRuns->load(std::memory_order_relaxed) },
                { "after_runs", afterInterruptRuns->load(std::memory_order_relaxed) },
            });
        }).isOk());
    assert(graph.addEdge(std::string(lc::START), "ask").isOk());
    assert(graph.addEdge("ask", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "compat-interrupt-rerun";
    options.checkpointer_ = checkpointer;

    auto paused = compiled->invoke(stateFromJson("{}"), options);
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(beforeInterruptRuns->load(std::memory_order_relaxed) == 1);
    assert(afterInterruptRuns->load(std::memory_order_relaxed) == 0);

    auto historyBeforeResume = compiled->getStateHistory("compat-interrupt-rerun", options);
    assert(historyBeforeResume.isOk());
    std::string beforeAskCheckpointId;
    for (const auto& snapshot : *historyBeforeResume) {
        if (snapshot.next_.size() == 1 && snapshot.next_.front() == "ask"
            && !snapshot.values_.view().contains("__interrupt__")) {
            beforeAskCheckpointId = snapshot.checkpointId_;
            break;
        }
    }
    assert(!beforeAskCheckpointId.empty());

    lc::RunOptions firstResume = options;
    firstResume.command_ = lc::Command::resume({ { "approval", "yes" } });
    auto completed = compiled->resume("compat-interrupt-rerun", firstResume);
    assert(completed.isOk());
    assert(completed->status_ == lc::RunStatus::Completed);
    assert(completed->state_.view().at("approved") == "yes");
    assert(completed->state_.view().at("before_runs") == 2);
    assert(completed->state_.view().at("after_runs") == 1);
    assert(beforeInterruptRuns->load(std::memory_order_relaxed) == 2);
    assert(afterInterruptRuns->load(std::memory_order_relaxed) == 1);

    auto replayed = compiled->replay("compat-interrupt-rerun", beforeAskCheckpointId, options);
    assert(replayed.isOk());
    assert(replayed->status_ == lc::RunStatus::Paused);
    assert(replayed->state_.view().at("__interrupt__").at("interrupts").front().at("id") == "approval");
    assert(beforeInterruptRuns->load(std::memory_order_relaxed) == 3);
    assert(afterInterruptRuns->load(std::memory_order_relaxed) == 1);

    lc::RunOptions secondResume = options;
    secondResume.command_ = lc::Command::resume({ { "approval", "again" } });
    auto replayCompleted = compiled->resume("compat-interrupt-rerun", secondResume);
    assert(replayCompleted.isOk());
    assert(replayCompleted->status_ == lc::RunStatus::Completed);
    assert(replayCompleted->state_.view().at("approved") == "again");
    assert(replayCompleted->state_.view().at("before_runs") == 4);
    assert(replayCompleted->state_.view().at("after_runs") == 2);
}

void testSequentialInterruptReplayAndResumeErrors()
{
    lc::StateGraph graph;
    assert(graph.addNode("ask", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto first = context.interrupt("first", { { "prompt", "first?" } });
        if (!first.isOk())
            return first.status();
        auto second = context.interrupt("second", { { "prompt", "second?" } });
        if (!second.isOk())
            return second.status();
        return lc::StateUpdate::fromJsonValue({
            { "first", *first },
            { "second", *second },
        });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "ask").isOk());
    assert(graph.addEdge("ask", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "compat-sequential-interrupt";
    options.checkpointer_ = checkpointer;

    auto firstPause = compiled->invoke(stateFromJson("{}"), options);
    assert(firstPause.isOk());
    assert(firstPause->status_ == lc::RunStatus::Paused);
    assert(firstPause->state_.view().at("__interrupt__").at("interrupts").front().at("id") == "first");

    lc::RunOptions firstResume = options;
    firstResume.command_ = lc::Command::resume({ { "first", "one" } });
    auto secondPause = compiled->resume("compat-sequential-interrupt", firstResume);
    assert(secondPause.isOk());
    assert(secondPause->status_ == lc::RunStatus::Paused);
    const auto& secondInterrupt = secondPause->state_.view().at("__interrupt__").at("interrupts").front();
    assert(secondInterrupt.at("id") == "second");
    assert(secondInterrupt.at("resume_values").at("first") == "one");

    auto secondSnapshot = compiled->getState("compat-sequential-interrupt", options);
    assert(secondSnapshot.isOk());
    auto replayed = compiled->replay("compat-sequential-interrupt", secondSnapshot->checkpointId_, options);
    assert(replayed.isOk());
    assert(replayed->status_ == lc::RunStatus::Paused);
    assert(replayed->state_.view().at("__interrupt__").at("interrupts").front().at("id") == "second");

    lc::RunOptions secondResume = options;
    secondResume.command_ = lc::Command::resume({ { "second", "two" } });
    auto completed = compiled->resume("compat-sequential-interrupt", secondResume);
    assert(completed.isOk());
    assert(completed->status_ == lc::RunStatus::Completed);
    assert(completed->state_.view().at("first") == "one");
    assert(completed->state_.view().at("second") == "two");

    lc::StateGraph multi;
    auto approvalNode = [](std::string id) {
        return [id = std::move(id)](const lc::State&, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
            if (!context.hasResumeValue()) {
                return lc::NodeOutput::interrupt(lc::Interrupt {
                    .id_ = id,
                    .value_ = { { "id", id } },
                });
            }
            auto update = lc::StateUpdate::fromJsonValue({ { id, context.resumeValue() } });
            if (!update.isOk())
                return update.status();
            return lc::NodeOutput::update(std::move(*update));
        };
    };
    assert(multi.addNode("left", approvalNode("left-int")).isOk());
    assert(multi.addNode("right", approvalNode("right-int")).isOk());
    assert(multi.addEdge(std::string(lc::START), "left").isOk());
    assert(multi.addEdge(std::string(lc::START), "right").isOk());
    assert(multi.addEdge("left", std::string(lc::END)).isOk());
    assert(multi.addEdge("right", std::string(lc::END)).isOk());
    auto compiledMulti = multi.compile();
    assert(compiledMulti.isOk());

    lc::RunOptions multiOptions;
    multiOptions.threadId_ = "compat-multi-interrupt-error";
    multiOptions.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    auto paused = compiledMulti->invoke(stateFromJson("{}"), multiOptions);
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    lc::RunOptions badResume = multiOptions;
    badResume.command_ = lc::Command::resume({ { "left-int", "L" } });
    auto missingPayload = compiledMulti->resume("compat-multi-interrupt-error", badResume);
    assert(!missingPayload.isOk());
    assert(missingPayload.status().code() == lc::StatusCode::FailedPrecondition);

    lc::RunOptions wrongIdOptions;
    wrongIdOptions.threadId_ = "compat-multi-interrupt-wrong-id";
    wrongIdOptions.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    auto wrongIdPaused = compiledMulti->invoke(stateFromJson("{}"), wrongIdOptions);
    assert(wrongIdPaused.isOk());
    assert(wrongIdPaused->status_ == lc::RunStatus::Paused);
    lc::RunOptions wrongIdResume = wrongIdOptions;
    wrongIdResume.command_ = lc::Command::resume({ { "unknown-int", "X" } });
    auto wrongPayload = compiledMulti->resume("compat-multi-interrupt-wrong-id", wrongIdResume);
    assert(!wrongPayload.isOk());
    assert(wrongPayload.status().code() == lc::StatusCode::FailedPrecondition);
}

void testSendAndCommandBoundaries()
{
    lc::StateGraph sendGraph;
    assert(sendGraph.addNode("fan", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(sendGraph.addNode("worker", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return lc::StateUpdate::fromJsonValue({ { "items", nlohmann::json::array({ state.view().at("item") }) } });
    }).isOk());
    assert(sendGraph.addEdge(std::string(lc::START), "fan").isOk());
    assert(sendGraph.addConditionalEdges(
        "fan",
        [](const lc::State&, lc::Runtime&) -> lc::Result<std::vector<lc::Send>> {
            return std::vector<lc::Send> {
                lc::Send("worker", stateFromJson(R"({"item":1})")),
                lc::Send("worker", stateFromJson(R"({"item":2})")),
            };
        },
        { "worker" }).isOk());
    assert(sendGraph.addEdge("worker", std::string(lc::END)).isOk());
    auto sendCompiled = sendGraph.compile();
    assert(sendCompiled.isOk());

    lc::RunOptions sendOptions;
    sendOptions.reducers_.set("items", lc::ReducerKind::Append);
    auto sendResult = sendCompiled->invoke(stateFromJson("{}"), sendOptions);
    assert(sendResult.isOk());
    assert(sendResult->state_.view().at("items").size() == 2);

    lc::StateGraph commandGraph;
    assert(commandGraph.addNode("decide", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
        return lc::NodeOutput::command(lc::Command::gotoNode(
            "finish",
            updateFromJson(R"({"routed":true})")));
    }).isOk());
    assert(commandGraph.addNode("finish", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        assert(state.view().at("routed") == true);
        return lc::StateUpdate::fromJson(R"({"finished":true})");
    }).isOk());
    assert(commandGraph.addEdge(std::string(lc::START), "decide").isOk());
    assert(commandGraph.addCommandRoute("decide", { "finish" }).isOk());
    assert(commandGraph.addEdge("finish", std::string(lc::END)).isOk());
    auto commandCompiled = commandGraph.compile();
    assert(commandCompiled.isOk());
    auto commandResult = commandCompiled->invoke(stateFromJson("{}"));
    assert(commandResult.isOk());
    assert(commandResult->state_.view().at("finished") == true);
}

void testSubgraphNamespaceAndStreamEnvelope()
{
    lc::StateGraph child;
    assert(child.addNode("child_node", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        assert(context.streamWriter().write("child-event", { { "inside", true } }).isOk());
        return lc::StateUpdate::fromJson(R"({"child":true})");
    }).isOk());
    assert(child.addEdge(std::string(lc::START), "child_node").isOk());
    assert(child.addEdge("child_node", std::string(lc::END)).isOk());
    auto compiledChild = child.compile();
    assert(compiledChild.isOk());
    auto childPtr = std::make_shared<lc::CompiledStateGraph>(std::move(*compiledChild));

    lc::StateGraph parent;
    assert(parent.addSubgraph("child", childPtr, lc::SubgraphOptions {
        .persistence_ = lc::SubgraphPersistenceMode::PerInvocation,
        .checkpointNamespace_ = "child",
    }).isOk());
    assert(parent.addEdge(std::string(lc::START), "child").isOk());
    assert(parent.addEdge("child", std::string(lc::END)).isOk());
    auto compiledParent = parent.compile();
    assert(compiledParent.isOk());

    lc::RunOptions options;
    options.threadId_ = "compat-subgraph";
    options.checkpointNamespace_ = "root";
    options.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    auto stream = compiledParent->streamProjected(
        stateFromJson("{}"),
        options,
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Events },
            .capacity_ = 64,
            .includeSubgraphs_ = true,
            .langGraphProtocol_ = true,
        });
    assert(stream.isOk());

    bool sawChildEnvelope = false;
    bool sawProtocolFields = false;
    auto parts = std::move(*stream);
    for (;;) {
        auto part = parts.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        const auto& data = (*part)->data_;
        assert(data.contains("event"));
        assert(data.contains("run_id"));
        assert(data.contains("parent_ids"));
        assert(data.contains("tags"));
        assert(data.contains("metadata"));
        assert(data.contains("data"));
        sawProtocolFields = sawProtocolFields
            || data.at("metadata").contains("event_id")
            || data.at("metadata").contains("sequence");
        if ((*part)->ns_.starts_with("root|child")) {
            sawChildEnvelope = true;
            assert(data.at("parent_ids").is_array());
            assert(!data.at("parent_ids").empty());
            assert(data.at("metadata").at("checkpoint_ns").get<std::string>().starts_with("root|child"));
            assert(data.at("metadata").at("trace_path").is_array());
        }
    }
    auto result = parts.result();
    assert(result.isOk());
    assert(sawChildEnvelope);
    assert(sawProtocolFields);
}

void testTokenStreamEnvelope()
{
    auto model = std::make_shared<lc::FakeChatModel>(
        std::vector<lc::BaseMessage> { lc::BaseMessage::ai("hello") });
    lc::StateGraph graph;
    assert(graph.addNode("model", lc::makeModelNode(model, lc::ModelNodeOptions {
        .stream_ = true,
    })).isOk());
    assert(graph.addEdge(std::string(lc::START), "model").isOk());
    assert(graph.addEdge("model", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto stream = compiled->streamProjected(
        stateFromJson(R"({"messages":[{"role":"user","content":"hi"}]})"),
        {},
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Events },
            .capacity_ = 32,
            .langGraphProtocol_ = true,
        });
    assert(stream.isOk());

    bool sawTokenChunk = false;
    auto parts = std::move(*stream);
    for (;;) {
        auto part = parts.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        const auto& envelope = (*part)->data_;
        if (envelope.at("event") == "on_chat_model_stream") {
            sawTokenChunk = true;
            assert(envelope.at("data").at("chunk").at("type") == "AIMessageChunk");
            assert(envelope.at("data").at("chunk").at("content") == "hello");
            assert(envelope.at("metadata").at("runtime_event_type") == "token");
        }
    }
    auto result = parts.result();
    assert(result.isOk());
    assert(sawTokenChunk);
}

void testStreamProjectionGoldenDetails()
{
    auto model = std::make_shared<lc::FakeChatModel>(
        std::vector<lc::BaseMessage> { lc::BaseMessage::ai("hello") });
    lc::StateGraph graph;
    assert(graph.addNode("model", lc::makeModelNode(model, lc::ModelNodeOptions {
        .stream_ = true,
    })).isOk());
    assert(graph.addEdge(std::string(lc::START), "model").isOk());
    assert(graph.addEdge("model", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.threadId_ = "compat-stream-projection";
    options.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);

    auto projected = compiled->streamProjected(
        stateFromMessages({ lc::BaseMessage::human("hi") }),
        options,
        lc::RunProjectionOptions {
            .modes_ = {
                lc::StreamMode::Updates,
                lc::StreamMode::Values,
                lc::StreamMode::Messages,
                lc::StreamMode::Tasks,
                lc::StreamMode::Checkpoints,
                lc::StreamMode::Debug,
                lc::StreamMode::Output,
            },
            .capacity_ = 64,
            .outputKeys_ = { "messages" },
        });
    assert(projected.isOk());

    bool sawUpdate = false;
    bool sawValue = false;
    bool sawMessageChunk = false;
    bool sawTaskStarted = false;
    bool sawTaskCompleted = false;
    bool sawCheckpoint = false;
    bool sawDebug = false;
    bool sawFilteredOutput = false;
    auto stream = std::move(*projected);
    for (;;) {
        auto part = stream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;

        if ((*part)->mode_ == lc::StreamMode::Updates) {
            sawUpdate = true;
            assert((*part)->data_.contains("model"));
            assert((*part)->data_.at("model").contains("messages"));
        }
        if ((*part)->mode_ == lc::StreamMode::Values) {
            sawValue = true;
            assert((*part)->data_.contains("messages"));
            assert(!(*part)->data_.contains("count"));
        }
        if ((*part)->mode_ == lc::StreamMode::Messages) {
            sawMessageChunk = true;
            assert((*part)->data_.at("text") == "hello");
            assert((*part)->data_.at("chunk").at("type") == "AIMessageChunk");
            assert((*part)->data_.at("chunk").at("content") == "hello");
            assert((*part)->data_.at("metadata").is_object());
            assert((*part)->data_.at("metadata").at("langgraph_node") == "model");
            assert((*part)->data_.at("event") == "content-block-delta");
            assert((*part)->data_.at("delta").at("type") == "text-delta");
        }
        if ((*part)->mode_ == lc::StreamMode::Tasks) {
            assert((*part)->data_.contains("id"));
            assert((*part)->data_.contains("name"));
            if ((*part)->data_.contains("input") && (*part)->data_.contains("triggers"))
                sawTaskStarted = true;
            if ((*part)->data_.contains("result") && (*part)->data_.contains("interrupts"))
                sawTaskCompleted = true;
        }
        if ((*part)->mode_ == lc::StreamMode::Checkpoints) {
            sawCheckpoint = true;
            assert((*part)->data_.contains("values"));
            assert((*part)->data_.contains("config"));
            assert((*part)->data_.at("config").at("configurable").at("checkpoint_id").is_string());
            assert((*part)->data_.contains("parent_config"));
            assert((*part)->data_.contains("next"));
            assert((*part)->data_.contains("tasks"));
            assert((*part)->data_.contains("metadata"));
            assert(!(*part)->data_.contains("checkpoint_id"));
            assert(!(*part)->data_.contains("next_tasks"));
            assert(!(*part)->data_.contains("pending_writes"));
        }
        if ((*part)->mode_ == lc::StreamMode::Debug) {
            sawDebug = true;
            assert((*part)->data_.at("step").is_number_unsigned());
            assert((*part)->data_.at("timestamp").is_string());
            assert((*part)->data_.contains("type"));
            assert((*part)->data_.contains("payload"));
        }
        if ((*part)->mode_ == lc::StreamMode::Output) {
            sawFilteredOutput = true;
            assert((*part)->data_.contains("messages"));
            assert(!(*part)->data_.contains("count"));
        }
    }

    auto result = stream.result();
    assert(result.isOk());
    assert(sawUpdate);
    assert(sawValue);
    assert(sawMessageChunk);
    assert(sawTaskStarted);
    assert(sawTaskCompleted);
    assert(sawCheckpoint);
    assert(sawDebug);
    assert(sawFilteredOutput);
}

void testStreamProjectionV2EnvelopeAndInterrupts()
{
    lc::StateGraph graph;
    assert(graph.addNode("tick", [](const lc::State& state, lc::Runtime&) {
        const auto count = state.view().value("count", 0);
        return lc::StateUpdate::fromJsonValue({ { "count", count + 1 } });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "tick").isOk());
    assert(graph.addEdge("tick", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.threadId_ = "compat-stream-v2";
    options.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    auto projected = compiled->streamProjected(
        stateFromJson(R"({"count":0})"),
        options,
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Updates, lc::StreamMode::Values },
            .capacity_ = 32,
            .version_ = lc::StreamProtocolVersion::V2,
        });
    assert(projected.isOk());

    bool sawUpdate = false;
    bool sawFinalValue = false;
    auto stream = std::move(*projected);
    for (;;) {
        auto part = stream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        const auto& data = (*part)->data_;
        assert(data.at("type") == ((*part)->mode_ == lc::StreamMode::Updates ? "updates" : "values"));
        assert(data.at("ns").is_array());
        assert(data.contains("data"));
        if ((*part)->mode_ == lc::StreamMode::Updates) {
            sawUpdate = true;
            assert(data.at("data").at("tick").at("count") == 1);
        }
        if ((*part)->mode_ == lc::StreamMode::Values
            && data.at("data").value("count", 0) == 1) {
            sawFinalValue = true;
            assert(data.at("interrupts").is_array());
            assert(data.at("interrupts").empty());
        }
    }
    auto result = stream.result();
    assert(result.isOk());
    assert(sawUpdate);
    assert(sawFinalValue);

    lc::StateGraph interruptGraph;
    assert(interruptGraph.addNode("pause", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto answer = context.interrupt("approval", { { "question", "continue?" } });
        if (!answer.isOk())
            return answer.status();
        return lc::StateUpdate::fromJsonValue({ { "approved", *answer } });
    }).isOk());
    assert(interruptGraph.addEdge(std::string(lc::START), "pause").isOk());
    assert(interruptGraph.addEdge("pause", std::string(lc::END)).isOk());
    auto compiledInterrupt = interruptGraph.compile();
    assert(compiledInterrupt.isOk());

    lc::RunOptions interruptOptions;
    interruptOptions.threadId_ = "compat-stream-v2-interrupt";
    interruptOptions.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    auto interruptProjected = compiledInterrupt->streamProjected(
        stateFromJson("{}"),
        interruptOptions,
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Values },
            .capacity_ = 32,
            .version_ = lc::StreamProtocolVersion::V2,
        });
    assert(interruptProjected.isOk());

    bool sawInterruptValue = false;
    auto interruptStream = std::move(*interruptProjected);
    for (;;) {
        auto part = interruptStream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        const auto& data = (*part)->data_;
        assert(data.at("type") == "values");
        assert(data.at("ns").is_array());
        assert(data.contains("data"));
        assert(data.contains("interrupts"));
        assert(!data.at("data").contains("__interrupt__"));
        if (!data.at("interrupts").empty()) {
            sawInterruptValue = true;
            assert(data.at("interrupts").front().at("id") == "approval");
            assert(data.at("interrupts").front().at("value").at("question") == "continue?");
        }
    }
    auto paused = interruptStream.result();
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(sawInterruptValue);
}

void testInterruptAndErrorStreamGoldenDetails()
{
    lc::StateGraph interruptGraph;
    assert(interruptGraph.addNode("pause", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto answer = context.interrupt("approval", { { "question", "approve?" } });
        if (!answer.isOk())
            return answer.status();
        return lc::StateUpdate::fromJsonValue({ { "approved", *answer } });
    }).isOk());
    assert(interruptGraph.addEdge(std::string(lc::START), "pause").isOk());
    assert(interruptGraph.addEdge("pause", std::string(lc::END)).isOk());
    auto compiledInterrupt = interruptGraph.compile();
    assert(compiledInterrupt.isOk());

    lc::RunOptions interruptOptions;
    interruptOptions.threadId_ = "compat-stream-interrupt";
    interruptOptions.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    auto interruptStreamResult = compiledInterrupt->streamProjected(
        stateFromJson("{}"),
        interruptOptions,
        lc::RunProjectionOptions {
            .modes_ = {
                lc::StreamMode::Events,
                lc::StreamMode::Interrupts,
                lc::StreamMode::Tasks,
            },
            .capacity_ = 64,
            .langGraphProtocol_ = true,
        });
    assert(interruptStreamResult.isOk());

    bool sawInterruptProjection = false;
    bool sawInterruptEnvelope = false;
    bool sawPausedTask = false;
    auto interruptStream = std::move(*interruptStreamResult);
    for (;;) {
        auto part = interruptStream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        if ((*part)->mode_ == lc::StreamMode::Interrupts) {
            sawInterruptProjection = true;
            assert((*part)->data_.at("id") == "approval");
            assert((*part)->data_.at("node") == "pause");
            assert((*part)->data_.at("value").at("question") == "approve?");
        }
        if ((*part)->mode_ == lc::StreamMode::Events
            && (*part)->data_.at("metadata").at("runtime_event_type") == "interrupt_requested") {
            sawInterruptEnvelope = true;
            assert((*part)->data_.at("event") == "on_custom_event");
            assert((*part)->data_.at("data").at("interrupt").at("id") == "approval");
        }
        if ((*part)->mode_ == lc::StreamMode::Tasks
            && (*part)->data_.contains("input")
            && (*part)->data_.contains("triggers")) {
            sawPausedTask = true;
        }
    }
    auto paused = interruptStream.result();
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(sawInterruptProjection);
    assert(sawInterruptEnvelope);
    assert(sawPausedTask);

    lc::StateGraph errorGraph;
    assert(errorGraph.addNode("boom", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return lc::Status::failedPrecondition("boom");
    }).isOk());
    assert(errorGraph.addEdge(std::string(lc::START), "boom").isOk());
    assert(errorGraph.addEdge("boom", std::string(lc::END)).isOk());
    auto compiledError = errorGraph.compile();
    assert(compiledError.isOk());

    lc::RunOptions errorOptions;
    errorOptions.threadId_ = "compat-stream-error";
    errorOptions.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    auto errorStreamResult = compiledError->streamProjected(
        stateFromJson("{}"),
        errorOptions,
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Events, lc::StreamMode::Tasks, lc::StreamMode::Errors },
            .capacity_ = 64,
            .langGraphProtocol_ = true,
        });
    assert(errorStreamResult.isOk());

    bool sawNodeErrorEnvelope = false;
    bool sawRunErrorEnvelope = false;
    bool sawFailedTask = false;
    bool sawErrorProjection = false;
    auto errorStream = std::move(*errorStreamResult);
    for (;;) {
        auto part = errorStream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        if ((*part)->mode_ == lc::StreamMode::Events && (*part)->data_.at("event") == "on_node_error") {
            sawNodeErrorEnvelope = true;
            assert((*part)->data_.at("data").at("error").at("message").get<std::string>().find("boom") != std::string::npos);
        }
        if ((*part)->mode_ == lc::StreamMode::Events && (*part)->data_.at("event") == "on_chain_error") {
            sawRunErrorEnvelope = true;
            assert((*part)->data_.at("data").at("error").at("message").get<std::string>().find("boom") != std::string::npos);
        }
        if ((*part)->mode_ == lc::StreamMode::Tasks
            && (*part)->data_.contains("error")
            && !(*part)->data_.at("error").is_null()) {
            sawFailedTask = true;
            assert((*part)->data_.at("error").get<std::string>().find("boom") != std::string::npos);
            assert((*part)->data_.at("result").is_object());
        }
        if ((*part)->mode_ == lc::StreamMode::Errors) {
            sawErrorProjection = true;
            assert((*part)->data_.at("error").at("message").get<std::string>().find("boom") != std::string::npos);
            assert((*part)->data_.contains("namespace"));
        }
    }
    auto failed = errorStream.result();
    assert(!failed.isOk());
    assert(failed.status().code() == lc::StatusCode::FailedPrecondition);
    assert(sawNodeErrorEnvelope);
    assert(sawRunErrorEnvelope);
    assert(sawFailedTask);
    assert(sawErrorProjection);
}

void testNestedSubgraphNamespaceHistoryAndParentCommand()
{
    lc::StateGraph leaf;
    assert(leaf.addNode("leaf_node", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        if (auto status = context.streamWriter().write("leaf-event", { { "leaf", true } }); !status.isOk())
            return status;
        return lc::StateUpdate::fromJsonValue({
            { "leaf_done", true },
            { "leaf_ns", std::string(context.executionInfo().checkpointNamespace_) },
        });
    }).isOk());
    assert(leaf.addEdge(std::string(lc::START), "leaf_node").isOk());
    assert(leaf.addEdge("leaf_node", std::string(lc::END)).isOk());
    auto compiledLeaf = leaf.compile();
    assert(compiledLeaf.isOk());

    lc::StateGraph middle;
    assert(middle.addSubgraph("leaf", std::make_shared<lc::CompiledStateGraph>(*compiledLeaf), lc::SubgraphOptions {
        .persistence_ = lc::SubgraphPersistenceMode::PerThread,
        .checkpointNamespace_ = "leaf",
    }).isOk());
    assert(middle.addEdge(std::string(lc::START), "leaf").isOk());
    assert(middle.addEdge("leaf", std::string(lc::END)).isOk());
    auto compiledMiddle = middle.compile();
    assert(compiledMiddle.isOk());

    lc::StateGraph parent;
    assert(parent.addSubgraph("mid", std::make_shared<lc::CompiledStateGraph>(*compiledMiddle), lc::SubgraphOptions {
        .persistence_ = lc::SubgraphPersistenceMode::PerThread,
        .checkpointNamespace_ = "mid",
    }).isOk());
    assert(parent.addEdge(std::string(lc::START), "mid").isOk());
    assert(parent.addEdge("mid", std::string(lc::END)).isOk());
    auto compiledParent = parent.compile();
    assert(compiledParent.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "compat-nested";
    options.checkpointNamespace_ = "root";
    options.checkpointer_ = checkpointer;
    auto streamResult = compiledParent->streamProjected(
        stateFromJson("{}"),
        options,
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Events, lc::StreamMode::Custom },
            .capacity_ = 64,
            .includeSubgraphs_ = true,
            .langGraphProtocol_ = true,
        });
    assert(streamResult.isOk());

    bool sawLeafCustom = false;
    bool sawLeafEnvelope = false;
    auto stream = std::move(*streamResult);
    for (;;) {
        auto part = stream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        if ((*part)->mode_ == lc::StreamMode::Custom && (*part)->name_ == "leaf-event") {
            sawLeafCustom = true;
            assert((*part)->ns_ == "root|mid|leaf");
        }
        if ((*part)->mode_ == lc::StreamMode::Events
            && (*part)->data_.at("metadata").at("runtime_event_type") == "custom"
            && (*part)->data_.at("name") == "leaf-event") {
            sawLeafEnvelope = true;
            assert((*part)->data_.at("metadata").at("checkpoint_ns") == "root|mid|leaf");
            assert((*part)->data_.at("parent_ids").is_array());
            assert((*part)->data_.at("parent_ids").size() >= 2);
            assert((*part)->data_.at("metadata").at("trace_path").is_array());
            assert((*part)->data_.at("metadata").at("trace_path").front() == "root");
            assert((*part)->data_.at("metadata").at("trace_path").back() == "root|mid|leaf");
            assert((*part)->data_.at("metadata").at("namespace").is_array());
            assert((*part)->data_.at("metadata").at("namespace").front() == "root");
            assert((*part)->data_.at("metadata").at("namespace").back() == "leaf");
        }
    }
    auto result = stream.result();
    assert(result.isOk());
    assert(result->state_.view().at("leaf_done") == true);
    assert(result->state_.view().at("leaf_ns") == "root|mid|leaf");
    assert(sawLeafCustom);
    assert(sawLeafEnvelope);

    auto middleHistory = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "compat-nested/mid",
        .checkpointNamespace_ = std::string("root|mid"),
        .order_ = lc::CheckpointListOrder::OldestFirst,
    });
    assert(middleHistory.isOk());
    assert(middleHistory->size() >= 2);
    auto leafHistory = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "compat-nested/mid/leaf",
        .checkpointNamespace_ = std::string("root|mid|leaf"),
        .order_ = lc::CheckpointListOrder::OldestFirst,
    });
    assert(leafHistory.isOk());
    assert(leafHistory->size() >= 2);

    auto parentOnlyHistory = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "compat-nested",
        .order_ = lc::CheckpointListOrder::OldestFirst,
    });
    assert(parentOnlyHistory.isOk());
    assert(!parentOnlyHistory->empty());
    for (const auto& record : *parentOnlyHistory)
        assert(record.checkpoint_.checkpointNamespace_ == "root");

    auto childRootHistory = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "compat-nested/mid/leaf",
        .checkpointNamespace_ = std::string("root"),
    });
    assert(childRootHistory.isOk());
    assert(childRootHistory->empty());

    lc::StateGraph commandChild;
    assert(commandChild.addNode("child_node", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
        return lc::NodeOutput::command(lc::Command::gotoParentNode(
            "after",
            updateFromJson(R"({"from_child":true})")));
    }).isOk());
    assert(commandChild.addEdge(std::string(lc::START), "child_node").isOk());
    auto compiledCommandChild = commandChild.compile();
    assert(compiledCommandChild.isOk());

    lc::StateGraph commandParent;
    assert(commandParent.addSubgraph("child", std::make_shared<lc::CompiledStateGraph>(*compiledCommandChild)).isOk());
    assert(commandParent.addNode("after", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        assert(state.view().at("from_child") == true);
        return lc::StateUpdate::fromJson(R"({"after":true})");
    }).isOk());
    assert(commandParent.addEdge(std::string(lc::START), "child").isOk());
    assert(commandParent.addCommandRoute("child", { "after" }).isOk());
    assert(commandParent.addEdge("after", std::string(lc::END)).isOk());
    auto compiledCommandParent = commandParent.compile();
    assert(compiledCommandParent.isOk());
    auto commandResult = compiledCommandParent->invoke(stateFromJson("{}"));
    assert(commandResult.isOk());
    assert(commandResult->state_.view().at("from_child") == true);
    assert(commandResult->state_.view().at("after") == true);
}

void testNestedSubgraphInterruptResumeAndHistory()
{
    auto leafBeforeInterruptRuns = std::make_shared<std::atomic<int>>(0);
    auto leafAfterInterruptRuns = std::make_shared<std::atomic<int>>(0);

    lc::StateGraph leaf;
    assert(leaf.addNode(
        "leaf_pause",
        [leafBeforeInterruptRuns, leafAfterInterruptRuns](
            const lc::State&,
            lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
            leafBeforeInterruptRuns->fetch_add(1, std::memory_order_relaxed);
            auto answer = context.interrupt(
                "leaf-approval",
                { { "scope", std::string(context.executionInfo().checkpointNamespace_) } });
            if (!answer.isOk())
                return answer.status();
            leafAfterInterruptRuns->fetch_add(1, std::memory_order_relaxed);
            return lc::StateUpdate::fromJsonValue({
                { "leaf_answer", *answer },
                { "leaf_ns", std::string(context.executionInfo().checkpointNamespace_) },
                { "leaf_before_runs", leafBeforeInterruptRuns->load(std::memory_order_relaxed) },
                { "leaf_after_runs", leafAfterInterruptRuns->load(std::memory_order_relaxed) },
            });
        }).isOk());
    assert(leaf.addEdge(std::string(lc::START), "leaf_pause").isOk());
    assert(leaf.addEdge("leaf_pause", std::string(lc::END)).isOk());
    auto compiledLeaf = leaf.compile();
    assert(compiledLeaf.isOk());

    lc::StateGraph middle;
    assert(middle.addSubgraph("leaf", std::make_shared<lc::CompiledStateGraph>(*compiledLeaf), lc::SubgraphOptions {
        .persistence_ = lc::SubgraphPersistenceMode::PerThread,
        .checkpointNamespace_ = "leaf",
    }).isOk());
    assert(middle.addEdge(std::string(lc::START), "leaf").isOk());
    assert(middle.addEdge("leaf", std::string(lc::END)).isOk());
    auto compiledMiddle = middle.compile();
    assert(compiledMiddle.isOk());

    lc::StateGraph parent;
    assert(parent.addSubgraph("mid", std::make_shared<lc::CompiledStateGraph>(*compiledMiddle), lc::SubgraphOptions {
        .persistence_ = lc::SubgraphPersistenceMode::PerThread,
        .checkpointNamespace_ = "mid",
    }).isOk());
    assert(parent.addEdge(std::string(lc::START), "mid").isOk());
    assert(parent.addEdge("mid", std::string(lc::END)).isOk());
    auto compiledParent = parent.compile();
    assert(compiledParent.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "compat-nested-interrupt";
    options.checkpointNamespace_ = "root";
    options.checkpointer_ = checkpointer;

    auto paused = compiledParent->invoke(stateFromJson("{}"), options);
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(leafBeforeInterruptRuns->load(std::memory_order_relaxed) == 1);
    assert(leafAfterInterruptRuns->load(std::memory_order_relaxed) == 0);

    const auto& parentInterrupt = paused->state_.view().at("__interrupt__").at("interrupts").front();
    assert(parentInterrupt.at("id") == "subgraph:mid");
    assert(parentInterrupt.at("node") == "mid");
    const auto& middleInterrupt = parentInterrupt.at("value").at("__interrupt__").at("interrupts").front();
    assert(middleInterrupt.at("id") == "subgraph:leaf");
    assert(middleInterrupt.at("node") == "leaf");
    const auto& leafInterrupt = middleInterrupt.at("value").at("__interrupt__").at("interrupts").front();
    assert(leafInterrupt.at("id") == "leaf-approval");
    assert(leafInterrupt.at("node") == "leaf_pause");
    assert(leafInterrupt.at("value").at("scope") == "root|mid|leaf");

    auto parentSnapshot = compiledParent->getState("compat-nested-interrupt", options);
    assert(parentSnapshot.isOk());
    assert(parentSnapshot->checkpointNamespace_ == "root");
    assert(parentSnapshot->next_.size() == 1);
    assert(parentSnapshot->next_.front() == "mid");
    assert(parentSnapshot->tasks_.size() == 1);
    assert(parentSnapshot->tasks_.front().nodeId_ == "mid");
    assert(parentSnapshot->tasks_.front().checkpointNamespace_ == "root");

    auto middleHistory = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "compat-nested-interrupt/mid",
        .checkpointNamespace_ = std::string("root|mid"),
        .order_ = lc::CheckpointListOrder::NewestFirst,
    });
    assert(middleHistory.isOk());
    assert(!middleHistory->empty());
    assert(middleHistory->front().checkpoint_.state_.view()
        .at("__interrupt__").at("interrupts").front().at("id") == "subgraph:leaf");

    auto leafHistory = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "compat-nested-interrupt/mid/leaf",
        .checkpointNamespace_ = std::string("root|mid|leaf"),
        .order_ = lc::CheckpointListOrder::NewestFirst,
    });
    assert(leafHistory.isOk());
    assert(!leafHistory->empty());
    assert(leafHistory->front().checkpoint_.state_.view()
        .at("__interrupt__").at("interrupts").front().at("id") == "leaf-approval");

    lc::RunOptions resumeOptions = options;
    resumeOptions.command_ = lc::Command::resume({ { "leaf-approval", "ok" } });
    auto completed = compiledParent->resume("compat-nested-interrupt", resumeOptions);
    assert(completed.isOk());
    assert(completed->status_ == lc::RunStatus::Completed);
    assert(completed->state_.view().at("leaf_answer") == "ok");
    assert(completed->state_.view().at("leaf_ns") == "root|mid|leaf");
    assert(completed->state_.view().at("leaf_before_runs") == 2);
    assert(completed->state_.view().at("leaf_after_runs") == 1);
    assert(leafBeforeInterruptRuns->load(std::memory_order_relaxed) == 2);
    assert(leafAfterInterruptRuns->load(std::memory_order_relaxed) == 1);
    assert(!completed->state_.view().contains("__interrupt__"));

    auto completedLeafHistory = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "compat-nested-interrupt/mid/leaf",
        .checkpointNamespace_ = std::string("root|mid|leaf"),
        .order_ = lc::CheckpointListOrder::NewestFirst,
    });
    assert(completedLeafHistory.isOk());
    assert(!completedLeafHistory->empty());
    assert(completedLeafHistory->front().checkpoint_.state_.view().at("leaf_answer") == "ok");
    assert(!completedLeafHistory->front().checkpoint_.state_.view().contains("__interrupt__"));
}

void testToolReturnedCommandAndToolInterrupt()
{
    auto routeRegistry = std::make_shared<lc::ToolRegistry>();
    auto routeRegistered = routeRegistry->add(std::make_shared<lc::FunctionTool>(
        lc::ToolSpec {
            .name_ = "route",
            .description_ = "route to finish",
        },
        [](const lc::ToolRequest&, lc::ToolRuntime&) -> lc::Result<lc::ToolResult> {
            return lc::ToolResult::command(
                lc::Command::gotoNode("finish", updateFromJson(R"({"tool_routed":true})")),
                { { "ok", true } });
        }));
    assert(routeRegistered.isOk());

    lc::StateGraph commandGraph;
    assert(commandGraph.addNode("tools", lc::ToolNode(routeRegistry)).isOk());
    assert(commandGraph.addNode("finish", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        assert(state.view().at("tool_routed") == true);
        assert(state.view().at("messages").size() == 2);
        return lc::StateUpdate::fromJson(R"({"finished":true})");
    }).isOk());
    assert(commandGraph.addEdge(std::string(lc::START), "tools").isOk());
    assert(commandGraph.addCommandRoute("tools", { "finish" }).isOk());
    assert(commandGraph.addEdge("finish", std::string(lc::END)).isOk());
    auto compiledCommand = commandGraph.compile();
    assert(compiledCommand.isOk());

    lc::RunOptions commandOptions;
    commandOptions.reducers_.set("messages", lc::ReducerKind::AddMessages);
    auto routed = compiledCommand->invoke(
        stateFromMessages({
            lc::BaseMessage::ai(
                "",
                { lc::ToolCall {
                    .id_ = "call-route",
                    .name_ = "route",
                    .args_ = nlohmann::json::object(),
                } }),
        }),
        commandOptions);
    assert(routed.isOk());
    assert(routed->state_.view().at("tool_routed") == true);
    assert(routed->state_.view().at("finished") == true);

    auto interruptRegistry = std::make_shared<lc::ToolRegistry>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto interruptRegistered = interruptRegistry->add(std::make_shared<lc::FunctionTool>(
        lc::ToolSpec {
            .name_ = "approve",
            .description_ = "interrupt before tool side effect",
        },
        [calls](const lc::ToolRequest& request, lc::ToolRuntime& context) -> lc::Result<lc::ToolResult> {
            calls->fetch_add(1, std::memory_order_relaxed);
            auto approved = context.interrupt("tool-approval", {
                { "call_id", request.callId_ },
                { "tool", request.name_ },
            });
            if (!approved.isOk())
                return approved.status();
            return lc::ToolResult::success({ { "approved", *approved } });
        }));
    assert(interruptRegistered.isOk());

    lc::StateGraph interruptGraph;
    assert(interruptGraph.addNode("tools", lc::ToolNode(interruptRegistry)).isOk());
    assert(interruptGraph.addEdge(std::string(lc::START), "tools").isOk());
    assert(interruptGraph.addEdge("tools", std::string(lc::END)).isOk());
    auto compiledInterrupt = interruptGraph.compile();
    assert(compiledInterrupt.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions interruptOptions;
    interruptOptions.threadId_ = "compat-tool-interrupt";
    interruptOptions.checkpointer_ = checkpointer;
    interruptOptions.reducers_.set("messages", lc::ReducerKind::AddMessages);
    auto paused = compiledInterrupt->invoke(
        stateFromMessages({
            lc::BaseMessage::ai(
                "",
                { lc::ToolCall {
                    .id_ = "call-approve",
                    .name_ = "approve",
                    .args_ = nlohmann::json::object(),
                } }),
        }),
        interruptOptions);
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(calls->load(std::memory_order_relaxed) == 1);
    const auto& interrupt = paused->state_.view().at("__interrupt__").at("interrupts").front();
    assert(interrupt.at("id") == "tool-approval");
    assert(interrupt.at("node") == "tools");
    assert(interrupt.at("value").at("call_id") == "call-approve");

    lc::RunOptions resumeOptions = interruptOptions;
    resumeOptions.command_ = lc::Command::resume({ { "tool-approval", true } });
    auto completed = compiledInterrupt->resume("compat-tool-interrupt", resumeOptions);
    assert(completed.isOk());
    assert(completed->status_ == lc::RunStatus::Completed);
    assert(calls->load(std::memory_order_relaxed) == 2);
    auto messages = lc::messagesFromStateJson(completed->state_.view());
    assert(messages.isOk());
    assert(messages->size() == 2);
    assert(messages->back().type_ == lc::MessageType::Tool);
    auto toolPayload = nlohmann::json::parse(messages->back().content_);
    assert(toolPayload.at("ok") == true);
    assert(toolPayload.at("result").at("approved") == true);
    assert(!completed->state_.view().contains("__interrupt__"));
}

} // namespace

int main()
{
    testRunnableConfigMergePatchAndEventMetadata();
    testSnapshotFieldsAndHistoryOrder();
    testInterruptReplay();
    testInterruptNodeRerunAndReplayAfterCompletion();
    testSequentialInterruptReplayAndResumeErrors();
    testSendAndCommandBoundaries();
    testSubgraphNamespaceAndStreamEnvelope();
    testTokenStreamEnvelope();
    testStreamProjectionGoldenDetails();
    testStreamProjectionV2EnvelopeAndInterrupts();
    testInterruptAndErrorStreamGoldenDetails();
    testNestedSubgraphNamespaceHistoryAndParentCommand();
    testNestedSubgraphInterruptResumeAndHistory();
    testToolReturnedCommandAndToolInterrupt();
    return 0;
}
