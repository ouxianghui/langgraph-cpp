#include <langgraph_cpp/langgraph.hpp>

#include "foundation/executor/inline_executor.hpp"

#if LANGGRAPH_CPP_WITH_SQLITE
#include "foundation/storage/sqlite_storage.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

lc::State stateFromJson(const char* text)
{
    auto state = lc::State::fromJson(text);
    assert(state.isOk());
    return *state;
}

lc::State stateFromMessages(std::vector<lc::BaseMessage> messages)
{
    auto state = lc::State::fromJsonValue({
        { "messages", lc::messagesToJson(messages) },
    });
    assert(state.isOk());
    return *state;
}

lc::StateUpdate updateFromJson(const char* text)
{
    auto update = lc::StateUpdate::fromJson(text);
    assert(update.isOk());
    return *update;
}

lc::Checkpoint checkpointFor(
    std::string threadId,
    std::string checkpointId,
    std::string checkpointNamespace,
    const char* stateJson,
    std::uint64_t step)
{
    return lc::Checkpoint {
        .threadId_ = std::move(threadId),
        .checkpointId_ = std::move(checkpointId),
        .checkpointNamespace_ = std::move(checkpointNamespace),
        .step_ = step,
        .state_ = stateFromJson(stateJson),
        .createdAt_ = std::chrono::system_clock::now(),
    };
}

lc::Result<std::optional<lc::Checkpoint>> latestCheckpoint(
    lc::BaseCheckpointSaver& checkpointer,
    std::string threadId,
    std::string checkpointNamespace = {})
{
    auto record = checkpointer.getTuple(lc::CheckpointQuery::latest(
        std::move(threadId),
        std::move(checkpointNamespace)));
    if (!record.isOk())
        return record.status();
    if (!record->has_value())
        return std::optional<lc::Checkpoint> {};
    return std::optional<lc::Checkpoint>((*record)->checkpoint_);
}

lc::Result<std::optional<lc::Checkpoint>> checkpointAt(
    lc::BaseCheckpointSaver& checkpointer,
    std::string threadId,
    std::string checkpointId,
    std::string checkpointNamespace = {})
{
    auto record = checkpointer.getTuple(lc::CheckpointQuery::at(
        std::move(threadId),
        std::move(checkpointId),
        std::move(checkpointNamespace)));
    if (!record.isOk())
        return record.status();
    if (!record->has_value())
        return std::optional<lc::Checkpoint> {};
    return std::optional<lc::Checkpoint>((*record)->checkpoint_);
}

lc::Result<std::vector<lc::Checkpoint>> checkpointHistory(
    lc::BaseCheckpointSaver& checkpointer,
    std::string threadId,
    std::optional<std::string> checkpointNamespace = std::string())
{
    auto records = checkpointer.list(lc::CheckpointListOptions {
        .threadId_ = std::move(threadId),
        .checkpointNamespace_ = std::move(checkpointNamespace),
        .order_ = lc::CheckpointListOrder::OldestFirst,
    });
    if (!records.isOk())
        return records.status();

    std::vector<lc::Checkpoint> checkpoints;
    checkpoints.reserve(records->size());
    for (auto& record : *records)
        checkpoints.push_back(std::move(record.checkpoint_));
    return checkpoints;
}

lc::CompiledStateGraph buildCountingGraph()
{
    lc::StateGraph graph;
    assert(graph.addNode("tick", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        return lc::StateUpdate::fromJsonValue({
            { "count", json->value("count", 0) + 1 },
        });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "tick").isOk());
    assert(graph.addConditionalEdges(
        "tick",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("count", 0) >= 3)
                return std::string(lc::END);
            return std::string("tick");
        },
        { "tick", std::string(lc::END) }).isOk());

    auto compiled = graph.compile();
    if (!compiled.isOk())
        std::cerr << compiled.status().toString() << '\n';
    assert(compiled.isOk());
    return *compiled;
}

void testReducers()
{
    lc::ReducerRegistry reducers;
    reducers
        .set("messages", lc::ReducerKind::AddMessages)
        .set("metadata", lc::ReducerKind::MergeObject);

    auto merged = lc::applyStateUpdate(
        stateFromJson(R"({"messages":[{"role":"user","content":"hi"}],"metadata":{"a":1},"status":"old","tags":["old"]})"),
        updateFromJson(R"({"messages":[{"role":"assistant","content":"hello"}],"metadata":{"b":2},"status":"new","tags":{"type":"__overwrite__","value":["fresh"]}})"),
        reducers);
    assert(merged.isOk());

    auto json = merged->toJson();
    assert(json.isOk());
    assert(json->at("messages").size() == 2);
    assert(json->at("metadata").at("a") == 1);
    assert(json->at("metadata").at("b") == 2);
    assert(json->at("status") == "new");
    assert(json->at("tags").size() == 1);
    assert(json->at("tags").at(0) == "fresh");

    auto replaced = lc::applyStateUpdate(
        stateFromJson(R"({"messages":[{"id":"m1","role":"assistant","content":"draft"}]})"),
        updateFromJson(R"({"messages":[{"id":"m1","role":"assistant","content":"final"},{"id":"m2","role":"user","content":"thanks"}]})"),
        reducers);
    assert(replaced.isOk());
    auto replacedJson = replaced->toJson();
    assert(replacedJson.isOk());
    assert(replacedJson->at("messages").size() == 2);
    assert(replacedJson->at("messages").at(0).at("content") == "final");
    assert(replacedJson->at("messages").at(1).at("id") == "m2");

    auto invalidAddMessages = lc::applyStateUpdate(
        stateFromJson(R"({"messages":"not-array"})"),
        updateFromJson(R"({"messages":["x"]})"),
        reducers);
    assert(!invalidAddMessages.isOk());
    assert(invalidAddMessages.status().code() == lc::StatusCode::InvalidArgument);
}

void testRunControl()
{
    lc::RunControl control;
    assert(!control.drainRequested());
    assert(!control.drainReason().has_value());

    control.requestDrain("maintenance");
    assert(control.drainRequested());
    auto reason = control.drainReason();
    assert(reason.has_value());
    assert(*reason == "maintenance");
}

void testMinimalGraphCheckpointAndEvents()
{
    lc::StateGraph graph;
    assert(graph.addNode("node", [](const lc::State&, lc::Runtime& context) {
        assert(context.previous().is_null());
        assert(context.executionInfo().runId_.starts_with("run-"));
        assert(context.executionInfo().threadId_ == "thread-test");
        assert(context.executionInfo().checkpointNamespace_.empty());
        assert(context.executionInfo().nodeId_ == "node");
        assert(context.executionInfo().step_ == 1);
        assert(!context.executionInfo().taskId_.empty());
        assert(context.executionInfo().nodeAttempt_ == 1);
        assert(!context.drainRequested());
        auto emitted = context.streamWriter().write("progress", { { "pct", 100 } });
        assert(emitted.isOk());
        return lc::StateUpdate::fromJson(R"({"ok":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "node").isOk());
    assert(graph.addEdge("node", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "thread-test";
    options.checkpointer_ = checkpointer;
    options.collectEvents_ = true;

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->threadId_ == "thread-test");
    assert(result->step_ == 1);

    auto finalJson = result->state_.toJson();
    assert(finalJson.isOk());
    assert(finalJson->at("ok") == true);

    auto checkpoints = checkpointHistory(*checkpointer, "thread-test");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 2);
    assert(checkpoints->front().step_ == 0);
    assert(checkpoints->back().step_ == 1);
    assert(checkpoints->back().nextNodes_.empty());

    bool sawCustom = false;
    bool sawStateUpdate = false;
    bool sawCheckpoint = false;
    for (const auto& event : result->events_) {
        sawCustom = sawCustom || event.type_ == lc::RuntimeEventType::Custom;
        sawStateUpdate = sawStateUpdate || event.type_ == lc::RuntimeEventType::StateUpdated;
        sawCheckpoint = sawCheckpoint || event.type_ == lc::RuntimeEventType::CheckpointCreated;
    }
    assert(sawCustom);
    assert(sawStateUpdate);
    assert(sawCheckpoint);
}

void testConditionalLoop()
{
    auto compiled = buildCountingGraph();
    auto result = compiled.invoke(stateFromJson(R"({"count":0})"));
    assert(result.isOk());
    assert(result->step_ == 3);

    auto finalJson = result->state_.toJson();
    assert(finalJson.isOk());
    assert(finalJson->at("count") == 3);
}

void testConditionalRouterFanoutAndFanIn()
{
    lc::StateGraph graph;
    assert(graph.addNode("route", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"decision":"inspect"})");
    }).isOk());
    assert(graph.addNode("temperature", [](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        assert(context.executionInfo().step_ == 2);
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        assert(json->at("decision") == "inspect");
        return lc::StateUpdate::fromJson(R"({"checks":["temperature"]})");
    }).isOk());
    assert(graph.addNode("power", [](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        assert(context.executionInfo().step_ == 2);
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        assert(json->at("decision") == "inspect");
        return lc::StateUpdate::fromJson(R"({"checks":["power"]})");
    }).isOk());
    assert(graph.addNode("join", [](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        assert(context.executionInfo().step_ == 3);
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        assert(json->at("checks") == nlohmann::json::array({ "temperature", "power" }));
        return lc::StateUpdate::fromJson(R"({"joined":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "route").isOk());
    assert(graph.addConditionalEdges(
        "route",
        [](const lc::State&, lc::Runtime&) -> lc::Result<std::vector<lc::NodeId>> {
            return std::vector<lc::NodeId> { "temperature", "power" };
        },
        { "temperature", "power" }).isOk());
    assert(graph.addEdge("temperature", "join").isOk());
    assert(graph.addEdge("power", "join").isOk());
    assert(graph.addEdge("join", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "conditional-fanout";
    options.checkpointer_ = checkpointer;
    options.reducers_.set("checks", lc::ReducerKind::Append);

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->step_ == 3);

    auto finalJson = result->state_.toJson();
    assert(finalJson.isOk());
    assert(finalJson->at("checks") == nlohmann::json::array({ "temperature", "power" }));
    assert(finalJson->at("joined") == true);

    auto checkpoints = checkpointHistory(*checkpointer, "conditional-fanout");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 4);
    assert(checkpoints->at(1).nextNodes_ == std::vector<std::string>({ "temperature", "power" }));
    assert(checkpoints->at(2).nextNodes_ == std::vector<std::string>({ "join" }));
    assert(checkpoints->back().nextNodes_.empty());
}

void testConditionalRouterRejectsUndeclaredFanoutTarget()
{
    lc::StateGraph graph;
    assert(graph.addNode("route", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.addNode("allowed", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.addNode("blocked", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "route").isOk());
    assert(graph.addEdge(std::string(lc::START), "blocked").isOk());
    assert(graph.addConditionalEdges(
        "route",
        [](const lc::State&, lc::Runtime&) -> lc::Result<std::vector<lc::NodeId>> {
            return std::vector<lc::NodeId> { "allowed", "blocked" };
        },
        { "allowed" }).isOk());
    assert(graph.addEdge("allowed", std::string(lc::END)).isOk());
    assert(graph.addEdge("blocked", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto result = compiled->invoke(stateFromJson("{}"));
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::FailedPrecondition);
}

lc::CompiledStateGraph buildSendMapReduceGraph()
{
    lc::StateGraph graph;
    assert(graph.addNode("plan", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"subjects":["edge","robot","sensor"],"topic":"runtime"})");
    }).isOk());
    assert(graph.addNode("generate", [](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        assert(context.executionInfo().step_ == 2 || context.executionInfo().step_ == 3);
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        assert(json->contains("subject"));
        assert(!json->contains("subjects"));
        const auto subject = json->at("subject").get<std::string>();
        return lc::StateUpdate::fromJsonValue({
            { "drafts", nlohmann::json::array({ subject + "-draft" }) },
            { "seen_subjects", nlohmann::json::array({ subject }) },
            { "last_subject", subject },
        });
    }).isOk());
    assert(graph.addNode("join", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        assert(json->at("drafts").size() == 3);
        return lc::StateUpdate::fromJsonValue({
            { "joined", true },
            { "draft_count", json->at("drafts").size() },
        });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "plan").isOk());
    assert(graph.addConditionalEdges(
        "plan",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<std::vector<lc::Send>> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();

            std::vector<lc::Send> sends;
            for (const auto& subject : json->at("subjects")) {
                auto branch = lc::State::fromJsonValue({
                    { "subject", subject },
                    { "topic", json->at("topic") },
                });
                if (!branch.isOk())
                    return branch.status();
                sends.push_back(lc::Send("generate", std::move(*branch)));
            }
            return sends;
        },
        { "generate" }).isOk());
    assert(graph.addEdge("generate", "join").isOk());
    assert(graph.addEdge("join", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    if (!compiled.isOk())
        std::cerr << compiled.status().toString() << '\n';
    assert(compiled.isOk());
    return *compiled;
}

void testSendFanoutUsesBranchStateAndFanIn()
{
    auto compiled = buildSendMapReduceGraph();

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "send-map-reduce";
    options.checkpointer_ = checkpointer;
    options.reducers_.set("drafts", lc::ReducerKind::Append);
    options.reducers_.set("seen_subjects", lc::ReducerKind::Append);

    auto result = compiled.invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->step_ == 3);

    auto json = result->state_.toJson();
    assert(json.isOk());
    assert(json->at("drafts") == nlohmann::json::array({ "edge-draft", "robot-draft", "sensor-draft" }));
    assert(json->at("seen_subjects") == nlohmann::json::array({ "edge", "robot", "sensor" }));
    assert(json->at("last_subject") == "sensor");
    assert(json->at("joined") == true);
    assert(json->at("draft_count") == 3);

    auto checkpoints = checkpointHistory(*checkpointer, "send-map-reduce");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 4);
    assert(checkpoints->at(1).nextNodes_ == std::vector<std::string>({ "generate", "generate", "generate" }));
    assert(checkpoints->at(1).nextTasks_.size() == 3);
    assert(checkpoints->at(1).nextTasks_[0].state_.has_value());
    assert(checkpoints->at(1).nextTasks_[0].state_->view().at("subject") == "edge");
    assert(checkpoints->at(2).nextNodes_ == std::vector<std::string>({ "join" }));
    assert(checkpoints->back().nextNodes_.empty());
}

void testResumeAfterSendCheckpoint()
{
    auto compiled = buildSendMapReduceGraph();

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions firstRun;
    firstRun.threadId_ = "send-resume";
    firstRun.checkpointer_ = checkpointer;
    firstRun.reducers_.set("drafts", lc::ReducerKind::Append);
    firstRun.reducers_.set("seen_subjects", lc::ReducerKind::Append);
    firstRun.limits_ = lc::ResourceLimits {}.maxSteps(1);

    auto stopped = compiled.invoke(stateFromJson("{}"), firstRun);
    assert(!stopped.isOk());
    assert(stopped.status().code() == lc::StatusCode::ResourceExhausted);

    auto latest = latestCheckpoint(*checkpointer, "send-resume");
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->nextNodes_ == std::vector<std::string>({ "generate", "generate", "generate" }));
    assert((*latest)->nextTasks_.size() == 3);
    assert((*latest)->nextTasks_[1].state_.has_value());
    assert((*latest)->nextTasks_[1].state_->view().at("subject") == "robot");

    lc::RunOptions resumeRun;
    resumeRun.checkpointer_ = checkpointer;
    resumeRun.reducers_.set("drafts", lc::ReducerKind::Append);
    resumeRun.reducers_.set("seen_subjects", lc::ReducerKind::Append);
    resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(10);

    auto resumed = compiled.resume("send-resume", resumeRun);
    assert(resumed.isOk());
    assert(resumed->step_ == 4);
    auto json = resumed->state_.toJson();
    assert(json.isOk());
    assert(json->at("drafts") == nlohmann::json::array({ "edge-draft", "robot-draft", "sensor-draft" }));
    assert(json->at("joined") == true);
    assert(!json->contains("__run_error__"));
}

void testCommandUpdateGoto()
{
    lc::StateGraph graph;
    assert(graph.addNode("decide", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
        auto update = lc::StateUpdate::fromJson(R"({"decision":"repair"})");
        if (!update.isOk())
            return update.status();
        return lc::NodeOutput::command(lc::Command::gotoNode("repair", std::move(*update)));
    }).isOk());
    assert(graph.addNode("repair", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        assert(json->at("decision") == "repair");
        return lc::StateUpdate::fromJson(R"({"repaired":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "decide").isOk());
    assert(graph.addCommandRoute("decide", { "repair" }).isOk());
    assert(graph.addEdge("repair", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "command-goto";
    options.checkpointer_ = checkpointer;

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->step_ == 2);
    auto json = result->state_.toJson();
    assert(json.isOk());
    assert(json->at("decision") == "repair");
    assert(json->at("repaired") == true);

    auto checkpoints = checkpointHistory(*checkpointer, "command-goto");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 3);
    assert(checkpoints->at(1).nextNodes_ == std::vector<std::string>({ "repair" }));
}

void testCommandRejectsUndeclaredGoto()
{
    lc::StateGraph graph;
    assert(graph.addNode("decide", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
        return lc::NodeOutput::command(lc::Command::gotoNode("blocked"));
    }).isOk());
    assert(graph.addNode("allowed", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.addNode("blocked", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"blocked_ran":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "decide").isOk());
    assert(graph.addEdge(std::string(lc::START), "blocked").isOk());
    assert(graph.addCommandRoute("decide", { "allowed" }).isOk());
    assert(graph.addEdge("allowed", std::string(lc::END)).isOk());
    assert(graph.addEdge("blocked", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto result = compiled->invoke(stateFromJson("{}"));
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::FailedPrecondition);
}

void testMaxConcurrencyCapsParallelNodes()
{
    std::atomic<int> running { 0 };
    std::atomic<int> maxRunning { 0 };

    auto node = [&](std::string name) {
        return [&, name = std::move(name)](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
            const int current = running.fetch_add(1, std::memory_order_acq_rel) + 1;
            int observed = maxRunning.load(std::memory_order_acquire);
            while (current > observed
                && !maxRunning.compare_exchange_weak(observed, current, std::memory_order_acq_rel)) {
            }
            if (current > 1) {
                running.fetch_sub(1, std::memory_order_acq_rel);
                return lc::Status::failedPrecondition("maxConcurrency did not cap active nodes");
            }
            running.fetch_sub(1, std::memory_order_acq_rel);
            return lc::StateUpdate::fromJsonValue({
                { "visited", nlohmann::json::array({ name }) },
            });
        };
    };

    lc::StateGraph graph;
    assert(graph.addNode("a", node("a")).isOk());
    assert(graph.addNode("b", node("b")).isOk());
    assert(graph.addNode("c", node("c")).isOk());
    assert(graph.addEdge(std::string(lc::START), "a").isOk());
    assert(graph.addEdge(std::string(lc::START), "b").isOk());
    assert(graph.addEdge(std::string(lc::START), "c").isOk());
    assert(graph.addEdge("a", std::string(lc::END)).isOk());
    assert(graph.addEdge("b", std::string(lc::END)).isOk());
    assert(graph.addEdge("c", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.maxConcurrency_ = 1;
    options.reducers_.set("visited", lc::ReducerKind::Append);

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(maxRunning.load(std::memory_order_acquire) == 1);
    auto json = result->state_.toJson();
    assert(json.isOk());
    assert(json->at("visited") == nlohmann::json::array({ "a", "b", "c" }));
}

void testCustomExecutorRunsParallelNodes()
{
    auto executor = lc::makeInlineExecutor();
    auto node = [executor](std::string name) {
        return [executor, name = std::move(name)](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
            assert(executor->isExecutorThread());
            return lc::StateUpdate::fromJsonValue({
                { "visited", nlohmann::json::array({ name }) },
            });
        };
    };

    lc::StateGraph graph;
    assert(graph.addNode("left", node("left")).isOk());
    assert(graph.addNode("right", node("right")).isOk());
    assert(graph.addEdge(std::string(lc::START), "left").isOk());
    assert(graph.addEdge(std::string(lc::START), "right").isOk());
    assert(graph.addEdge("left", std::string(lc::END)).isOk());
    assert(graph.addEdge("right", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.executor_ = executor;
    options.reducers_.set("visited", lc::ReducerKind::Append);

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    auto json = result->state_.toJson();
    assert(json.isOk());
    assert(json->at("visited") == nlohmann::json::array({ "left", "right" }));
}

void testParallelFanoutAndFanIn()
{
    std::mutex mutex;
    std::condition_variable ready;
    int started = 0;

    auto branch = [&](std::string name) {
        return [&, name = std::move(name)](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
            assert(context.executionInfo().step_ == 1);
            auto input = state.toJson();
            if (!input.isOk())
                return input.status();
            assert(!input->contains("seen"));

            std::unique_lock lock(mutex);
            ++started;
            ready.notify_all();
            if (!ready.wait_for(lock, std::chrono::seconds(2), [&] { return started == 2; }))
                return lc::Status::deadlineExceeded("parallel fan-out peer did not start");
            lock.unlock();

            return lc::StateUpdate::fromJsonValue({
                { "seen", nlohmann::json::array({ name }) },
                { "winner", name },
            });
        };
    };

    lc::StateGraph graph;
    assert(graph.addNode("left", branch("left")).isOk());
    assert(graph.addNode("right", branch("right")).isOk());
    assert(graph.addNode("join", [](const lc::State& state, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        assert(context.executionInfo().step_ == 2);
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        assert(json->at("seen").size() == 2);
        return lc::StateUpdate::fromJsonValue({
            { "joined", true },
            { "seen_at_join", json->at("seen") },
            { "winner_at_join", json->at("winner") },
        });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "left").isOk());
    assert(graph.addEdge(std::string(lc::START), "right").isOk());
    assert(graph.addEdge("left", "join").isOk());
    assert(graph.addEdge("right", "join").isOk());
    assert(graph.addEdge("join", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "parallel-fanout";
    options.checkpointer_ = checkpointer;
    options.reducers_.set("seen", lc::ReducerKind::Append);

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->step_ == 2);

    auto finalJson = result->state_.toJson();
    assert(finalJson.isOk());
    assert(finalJson->at("seen") == nlohmann::json::array({ "left", "right" }));
    assert(finalJson->at("winner") == "right");
    assert(finalJson->at("joined") == true);
    assert(finalJson->at("seen_at_join") == nlohmann::json::array({ "left", "right" }));
    assert(finalJson->at("winner_at_join") == "right");

    auto checkpoints = checkpointHistory(*checkpointer, "parallel-fanout");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 3);
    assert(checkpoints->front().nextNodes_ == std::vector<std::string>({ "left", "right" }));
    assert(checkpoints->at(1).step_ == 1);
    assert(checkpoints->at(1).writes_.size() == 2);
    assert(checkpoints->at(1).nextNodes_ == std::vector<std::string>({ "join" }));
    assert(checkpoints->back().nextNodes_.empty());
}

void testResumeAfterFanoutCheckpoint()
{
    lc::StateGraph graph;
    assert(graph.addNode("left", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"values":["left"]})");
    }).isOk());
    assert(graph.addNode("right", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"values":["right"]})");
    }).isOk());
    assert(graph.addNode("join", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"joined":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "left").isOk());
    assert(graph.addEdge(std::string(lc::START), "right").isOk());
    assert(graph.addEdge("left", "join").isOk());
    assert(graph.addEdge("right", "join").isOk());
    assert(graph.addEdge("join", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions firstRun;
    firstRun.threadId_ = "parallel-resume";
    firstRun.checkpointer_ = checkpointer;
    firstRun.reducers_.set("values", lc::ReducerKind::Append);
    firstRun.limits_ = lc::ResourceLimits {}.maxSteps(1);

    auto stopped = compiled->invoke(stateFromJson("{}"), firstRun);
    assert(!stopped.isOk());
    assert(stopped.status().code() == lc::StatusCode::ResourceExhausted);

    auto latest = latestCheckpoint(*checkpointer, "parallel-resume");
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->nextNodes_ == std::vector<std::string>({ "join" }));

    lc::RunOptions resumeRun;
    resumeRun.checkpointer_ = checkpointer;
    resumeRun.reducers_.set("values", lc::ReducerKind::Append);
    resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(10);

    auto resumed = compiled->resume("parallel-resume", resumeRun);
    assert(resumed.isOk());
    assert(resumed->step_ == 3);
    auto finalJson = resumed->state_.toJson();
    assert(finalJson.isOk());
    assert(finalJson->at("values") == nlohmann::json::array({ "left", "right" }));
    assert(finalJson->at("joined") == true);
    assert(!finalJson->contains("__run_error__"));
}

void testStorageSaverAndResume()
{
    auto compiled = buildCountingGraph();
    auto storage = std::make_shared<lc::MemoryStorage>();
    auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

    lc::RunOptions firstRun;
    firstRun.threadId_ = "thread-resume";
    firstRun.checkpointer_ = checkpointer;
    firstRun.limits_ = lc::ResourceLimits {}.maxSteps(2);

    auto stopped = compiled.invoke(stateFromJson(R"({"count":0})"), firstRun);
    assert(!stopped.isOk());
    assert(stopped.status().code() == lc::StatusCode::ResourceExhausted);

    auto latest = latestCheckpoint(*checkpointer, "thread-resume");
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->step_ == 3);
    assert((*latest)->nextNodes_ == std::vector<std::string> { "tick" });

    lc::RunOptions resumeRun;
    resumeRun.checkpointer_ = checkpointer;
    resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(10);

    auto resumed = compiled.resume("thread-resume", resumeRun);
    assert(resumed.isOk());
    assert(resumed->threadId_ == "thread-resume");
    assert(resumed->step_ == 4);

    auto finalJson = resumed->state_.toJson();
    assert(finalJson.isOk());
    assert(finalJson->at("count") == 3);

    auto checkpoints = checkpointHistory(*checkpointer, "thread-resume");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 5);
    assert(checkpoints->back().step_ == 4);
    assert(checkpoints->back().nextNodes_.empty());

    auto completedAgain = compiled.resume("thread-resume", resumeRun);
    assert(completedAgain.isOk());
    assert(completedAgain->step_ == 4);
    auto completedJson = completedAgain->state_.toJson();
    assert(completedJson.isOk());
    assert(completedJson->at("count") == 3);
    auto unchanged = checkpointHistory(*checkpointer, "thread-resume");
    assert(unchanged.isOk());
    assert(unchanged->size() == 5);
}

void testCheckpointNamespaceIsQueryDimension()
{
    auto exercise = [](lc::BaseCheckpointSaver& checkpointer) {
        assert(checkpointer.put(checkpointFor("thread-ns", "shared-id", "", R"({"scope":"root"})", 1)).isOk());
        assert(checkpointer.put(checkpointFor("thread-ns", "shared-id", "child", R"({"scope":"child"})", 2)).isOk());

        auto rootLatest = latestCheckpoint(checkpointer, "thread-ns");
        assert(rootLatest.isOk());
        assert(rootLatest->has_value());
        assert((*rootLatest)->checkpointNamespace_.empty());
        assert((*rootLatest)->state_.view().at("scope") == "root");

        auto childLatest = latestCheckpoint(checkpointer, "thread-ns", "child");
        assert(childLatest.isOk());
        assert(childLatest->has_value());
        assert((*childLatest)->checkpointNamespace_ == "child");
        assert((*childLatest)->state_.view().at("scope") == "child");

        auto rootById = checkpointAt(checkpointer, "thread-ns", "shared-id");
        assert(rootById.isOk());
        assert(rootById->has_value());
        assert((*rootById)->state_.view().at("scope") == "root");

        auto childById = checkpointAt(checkpointer, "thread-ns", "shared-id", "child");
        assert(childById.isOk());
        assert(childById->has_value());
        assert((*childById)->state_.view().at("scope") == "child");

        auto rootHistory = checkpointHistory(checkpointer, "thread-ns");
        assert(rootHistory.isOk());
        assert(rootHistory->size() == 1);
        assert(rootHistory->front().checkpointNamespace_.empty());

        auto childHistory = checkpointHistory(checkpointer, "thread-ns", "child");
        assert(childHistory.isOk());
        assert(childHistory->size() == 1);
        assert(childHistory->front().checkpointNamespace_ == "child");
    };

    lc::InMemorySaver memoryCheckpointer;
    exercise(memoryCheckpointer);

    auto storage = std::make_shared<lc::MemoryStorage>();
    lc::StorageSaver storageCheckpointer(storage);
    exercise(storageCheckpointer);
}

#if LANGGRAPH_CPP_WITH_SQLITE
void testSQLiteStorageSaverResume()
{
    const auto dbPath = std::filesystem::temp_directory_path()
        / ("langgraph-cpp-resume-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + ".sqlite");
    std::filesystem::remove(dbPath);

    auto compiled = buildCountingGraph();

    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

        lc::RunOptions firstRun;
        firstRun.threadId_ = "sqlite-thread-resume";
        firstRun.checkpointer_ = checkpointer;
        firstRun.limits_ = lc::ResourceLimits {}.maxSteps(2);

        auto stopped = compiled.invoke(stateFromJson(R"({"count":0})"), firstRun);
        assert(!stopped.isOk());
        assert(stopped.status().code() == lc::StatusCode::ResourceExhausted);
        assert(storage->close().isOk());
    }

    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

        lc::RunOptions resumeRun;
        resumeRun.checkpointer_ = checkpointer;
        resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(10);

        auto resumed = compiled.resume("sqlite-thread-resume", resumeRun);
        assert(resumed.isOk());
        auto json = resumed->state_.toJson();
        assert(json.isOk());
        assert(json->at("count") == 3);
        assert(storage->close().isOk());
    }

    std::filesystem::remove(dbPath);
}

void testSQLiteCheckpointNamespaceResumeAfterReopen()
{
    const auto dbPath = std::filesystem::temp_directory_path()
        / ("langgraph-cpp-ns-resume-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + ".sqlite");
    std::filesystem::remove(dbPath);

    auto compiled = buildCountingGraph();

    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

        lc::RunOptions firstRun;
        firstRun.threadId_ = "sqlite-ns-thread";
        firstRun.checkpointNamespace_ = "root";
        firstRun.checkpointer_ = checkpointer;
        firstRun.limits_ = lc::ResourceLimits {}.maxSteps(2);

        auto stopped = compiled.invoke(stateFromJson(R"({"count":0})"), firstRun);
        assert(!stopped.isOk());
        assert(stopped.status().code() == lc::StatusCode::ResourceExhausted);
        assert(storage->close().isOk());
    }

    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

        auto rootLatest = latestCheckpoint(*checkpointer, "sqlite-ns-thread");
        assert(rootLatest.isOk());
        assert(!rootLatest->has_value());

        auto namespacedLatest = latestCheckpoint(*checkpointer, "sqlite-ns-thread", "root");
        assert(namespacedLatest.isOk());
        assert(namespacedLatest->has_value());
        assert((*namespacedLatest)->checkpointNamespace_ == "root");

        lc::RunOptions resumeRun;
        resumeRun.checkpointNamespace_ = "root";
        resumeRun.checkpointer_ = checkpointer;
        resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(10);

        auto resumed = compiled.resume("sqlite-ns-thread", resumeRun);
        assert(resumed.isOk());
        assert(resumed->state_.view().at("count") == 3);
        assert(storage->close().isOk());
    }

    std::filesystem::remove(dbPath);
}

void testSQLiteSyncDurabilityPendingWritesReplayAfterReopen()
{
    const auto dbPath = std::filesystem::temp_directory_path()
        / ("langgraph-cpp-sync-replay-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + ".sqlite");
    std::filesystem::remove(dbPath);

    std::atomic<int> aRuns { 0 };
    std::atomic<int> bRuns { 0 };
    std::atomic<int> cRuns { 0 };

    lc::StateGraph graph;
    assert(graph.addNode("a", [&](const lc::State&, lc::Runtime&) {
        ++aRuns;
        return lc::StateUpdate::fromJson(R"({"visited":["a"]})");
    }).isOk());
    assert(graph.addNode("b", [&](const lc::State&, lc::Runtime&) {
        ++bRuns;
        return lc::StateUpdate::fromJson(R"({"visited":["b"]})");
    }).isOk());
    assert(graph.addNode("c", [&](const lc::State&, lc::Runtime&) {
        ++cRuns;
        return lc::StateUpdate::fromJson(R"({"visited":["c"]})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "a").isOk());
    assert(graph.addEdge(std::string(lc::START), "b").isOk());
    assert(graph.addEdge(std::string(lc::START), "c").isOk());
    assert(graph.addEdge("a", std::string(lc::END)).isOk());
    assert(graph.addEdge("b", std::string(lc::END)).isOk());
    assert(graph.addEdge("c", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

        lc::RunOptions options;
        options.threadId_ = "sqlite-sync-durable";
        options.checkpointNamespace_ = "root";
        options.checkpointer_ = checkpointer;
        options.durability_ = lc::Durability::Sync;
        options.maxConcurrency_ = 1;
        options.reducers_.set("visited", lc::ReducerKind::Append);

        auto result = compiled->invoke(stateFromJson("{}"), options);
        assert(result.isOk());
        assert(result->state_.view().at("visited") == nlohmann::json::array({ "a", "b", "c" }));
        assert(storage->close().isOk());
    }

    std::string taskWriteCheckpointId;
    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);
        auto history = checkpointHistory(*checkpointer, "sqlite-sync-durable", "root");
        assert(history.isOk());
        for (const auto& checkpoint : *history) {
            if (checkpoint.metadata_.value("source", "") == "task_writes"
                && checkpoint.pendingWrites_.size() == 1) {
                taskWriteCheckpointId = checkpoint.checkpointId_;
                break;
            }
        }
        assert(!taskWriteCheckpointId.empty());
        assert(storage->close().isOk());
    }

    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

        lc::RunOptions replayOptions;
        replayOptions.checkpointNamespace_ = "root";
        replayOptions.checkpointer_ = checkpointer;
        replayOptions.durability_ = lc::Durability::Sync;
        replayOptions.maxConcurrency_ = 1;
        replayOptions.reducers_.set("visited", lc::ReducerKind::Append);

        auto replayed = compiled->replay("sqlite-sync-durable", taskWriteCheckpointId, replayOptions);
        assert(replayed.isOk());
        assert(aRuns.load(std::memory_order_acquire) == 1);
        assert(bRuns.load(std::memory_order_acquire) == 2);
        assert(cRuns.load(std::memory_order_acquire) == 2);
        assert(replayed->state_.view().at("visited") == nlohmann::json::array({ "a", "b", "c" }));
        assert(storage->close().isOk());
    }

    std::filesystem::remove(dbPath);
}
#endif

void testMaxSteps()
{
    lc::StateGraph graph;
    assert(graph.addNode("tick", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "tick").isOk());
    assert(graph.addConditionalEdges(
        "tick",
        [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeId> {
            return std::string("tick");
        }).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.threadId_ = "max-step-thread";
    options.checkpointer_ = std::make_shared<lc::InMemorySaver>();
    options.limits_ = lc::ResourceLimits {}.maxSteps(2);
    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(!result.isOk());
    assert(result.status().code() == lc::StatusCode::ResourceExhausted);
    assert(lc::runStatusFromStatus(result.status()) == lc::RunStatus::MaxStepsExceeded);

    auto checkpoints = checkpointHistory(*options.checkpointer_, "max-step-thread");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 4);
    assert(checkpoints->back().nextNodes_ == std::vector<std::string> { "tick" });
    auto failedState = checkpoints->back().state_.toJson();
    assert(failedState.isOk());
    assert(failedState->contains("__run_error__"));
    assert(failedState->at("__run_error__").at("status") == "max_steps_exceeded");
    assert(failedState->at("__run_error__").at("node") == "tick");
    assert(failedState->at("__run_error__").at("step") == 3);
}

void testInterruptAndCommandResume()
{
    lc::StateGraph graph;
    assert(graph.addNode("approve", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
        if (!context.hasResumeValue()) {
            return lc::NodeOutput::interrupt(lc::Interrupt {
                .id_ = "approval",
                .value_ = {
                    { "question", "continue?" },
                },
            });
        }

        auto update = lc::StateUpdate::fromJsonValue({
            { "approved", context.resumeValue().value("approved", false) },
        });
        if (!update.isOk())
            return update.status();
        return lc::NodeOutput::update(std::move(*update));
    }).isOk());
    assert(graph.addNode("finish", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"done":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "approve").isOk());
    assert(graph.addConditionalEdges(
        "approve",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("approved", false))
                return std::string("finish");
            return std::string(lc::END);
        },
        { "finish", std::string(lc::END) }).isOk());
    assert(graph.addEdge("finish", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "interrupt-thread";
    options.checkpointer_ = checkpointer;
    options.collectEvents_ = true;

    auto paused = compiled->invoke(stateFromJson("{}"), options);
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(paused->step_ == 1);

    auto checkpoints = checkpointHistory(*checkpointer, "interrupt-thread");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 2);
    assert(checkpoints->back().nextNodes_ == std::vector<std::string> { "approve" });
    auto pausedState = checkpoints->back().state_.toJson();
    assert(pausedState.isOk());
    assert(pausedState->contains("__interrupt__"));
    assert(pausedState->at("__interrupt__").at("interrupts").size() == 1);
    assert(pausedState->at("__interrupt__").at("interrupts").front().at("id") == "approval");
    assert(pausedState->at("__interrupt__").at("interrupts").front().at("node") == "approve");
    assert(pausedState->at("__interrupt__").at("interrupts").front().at("value").at("question") == "continue?");

    bool sawInterrupt = false;
    for (const auto& event : paused->events_) {
        sawInterrupt = sawInterrupt || event.type_ == lc::RuntimeEventType::InterruptRequested;
    }
    assert(sawInterrupt);

    lc::RunOptions missingCommand;
    missingCommand.checkpointer_ = checkpointer;
    auto missingCommandResume = compiled->resume("interrupt-thread", missingCommand);
    assert(!missingCommandResume.isOk());
    assert(missingCommandResume.status().code() == lc::StatusCode::FailedPrecondition);

    lc::RunOptions resumeOptions;
    resumeOptions.checkpointer_ = checkpointer;
    resumeOptions.command_ = lc::Command::resume({
        { "approved", true },
    });

    auto resumed = compiled->resume("interrupt-thread", resumeOptions);
    assert(resumed.isOk());
    assert(resumed->status_ == lc::RunStatus::Completed);
    assert(resumed->step_ == 3);
    auto json = resumed->state_.toJson();
    assert(json.isOk());
    assert(json->at("approved") == true);
    assert(json->at("done") == true);
    assert(!json->contains("__interrupt__"));
}

void testStreamFilteringAndWriter()
{
    lc::StateGraph graph;
    assert(graph.addNode("node", [](const lc::State&, lc::Runtime& context) {
        auto status = context.streamWriter().write("progress", {
            { "pct", 50 },
        });
        assert(status.isOk());
        return lc::StateUpdate::fromJson(R"({"ok":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "node").isOk());
    assert(graph.addEdge("node", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    std::vector<lc::RuntimeEventType> callbackTypes;
    lc::RunOptions options = lc::RunOptions::onlyEvents({ lc::RuntimeEventType::Custom });
    options.eventCallback_ = [&](lc::RuntimeEvent event) {
        callbackTypes.push_back(event.type_);
        return lc::Status::ok();
    };

    auto result = compiled->stream(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->events_.size() == 1);
    assert(result->events_.front().type_ == lc::RuntimeEventType::Custom);
    assert(result->events_.front().name_ == "progress");
    assert(callbackTypes.size() == 1);
    assert(callbackTypes.front() == lc::RuntimeEventType::Custom);
}

void testStateSnapshotReplayAndUpdateStateFork()
{
    auto compiled = buildCountingGraph();
    auto checkpointer = std::make_shared<lc::InMemorySaver>();

    lc::RunOptions options;
    options.threadId_ = "time-travel";
    options.checkpointer_ = checkpointer;

    auto result = compiled.invoke(stateFromJson(R"({"count":0})"), options);
    assert(result.isOk());
    assert(result->state_.view().at("count") == 3);

    auto latest = compiled.getState("time-travel", options);
    assert(latest.isOk());
    assert(latest->step_ == 3);
    assert(latest->next_.empty());
    assert(latest->values_.view().at("count") == 3);

    auto history = compiled.getStateHistory("time-travel", options);
    assert(history.isOk());
    assert(history->size() == 4);
    assert(history->front().step_ == 3);
    assert(history->back().step_ == 0);
    const auto stepOne = std::find_if(
        history->begin(),
        history->end(),
        [](const lc::StateSnapshot& snapshot) {
            return snapshot.step_ == 1;
        });
    assert(stepOne != history->end());
    assert(stepOne->next_ == std::vector<std::string>({ "tick" }));
    assert(stepOne->values_.view().at("count") == 1);

    auto replayed = compiled.replay("time-travel", stepOne->checkpointId_, options);
    assert(replayed.isOk());
    assert(replayed->state_.view().at("count") == 3);
    assert(replayed->step_ > result->step_);

    lc::StateUpdateOptions updateOptions;
    updateOptions.checkpointId_ = stepOne->checkpointId_;
    updateOptions.asNode_ = "tick";
    auto forked = compiled.updateState(
        "time-travel",
        updateFromJson(R"({"count":10})"),
        options,
        updateOptions);
    assert(forked.isOk());
    assert(forked->parentCheckpointId_ == stepOne->checkpointId_);
    assert(forked->values_.view().at("count") == 10);
    assert(forked->next_.empty());

    auto forkLatest = compiled.getState("time-travel", options);
    assert(forkLatest.isOk());
    assert(forkLatest->checkpointId_ == forked->checkpointId_);
    assert(forkLatest->values_.view().at("count") == 10);
}

void testPendingWritesSkipCompletedParallelNodesOnResume()
{
    std::atomic<int> aRuns { 0 };
    std::atomic<int> bRuns { 0 };
    std::atomic<int> cRuns { 0 };

    lc::StateGraph graph;
    assert(graph.addNode("a", [&](const lc::State&, lc::Runtime&) {
        ++aRuns;
        return lc::StateUpdate::fromJson(R"({"visited":["a"]})");
    }).isOk());
    assert(graph.addNode("b", [&](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        const int attempt = ++bRuns;
        if (attempt == 1)
            return lc::Status::internal("b failed once");
        return lc::StateUpdate::fromJson(R"({"visited":["b"]})");
    }).isOk());
    assert(graph.addNode("c", [&](const lc::State&, lc::Runtime&) {
        ++cRuns;
        return lc::StateUpdate::fromJson(R"({"visited":["c"]})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "a").isOk());
    assert(graph.addEdge(std::string(lc::START), "b").isOk());
    assert(graph.addEdge(std::string(lc::START), "c").isOk());
    assert(graph.addEdge("a", std::string(lc::END)).isOk());
    assert(graph.addEdge("b", std::string(lc::END)).isOk());
    assert(graph.addEdge("c", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "pending-writes";
    options.checkpointer_ = checkpointer;
    options.reducers_.set("visited", lc::ReducerKind::Append);

    auto failed = compiled->invoke(stateFromJson("{}"), options);
    assert(!failed.isOk());
    assert(aRuns.load() == 1);
    assert(bRuns.load() == 1);
    assert(cRuns.load() == 1);

    auto latest = latestCheckpoint(*checkpointer, "pending-writes");
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->nextNodes_ == std::vector<std::string>({ "b" }));
    assert((*latest)->pendingWrites_.size() == 2);
    assert((*latest)->pendingWrites_[0].nodeId_ == "a");
    assert((*latest)->pendingWrites_[1].nodeId_ == "c");

    auto resumed = compiled->resume("pending-writes", options);
    assert(resumed.isOk());
    assert(aRuns.load() == 1);
    assert(bRuns.load() == 2);
    assert(cRuns.load() == 1);
    assert(resumed->state_.view().at("visited") == nlohmann::json::array({ "a", "b", "c" }));

    auto finalSnapshot = compiled->getState("pending-writes", options);
    assert(finalSnapshot.isOk());
    assert(finalSnapshot->pendingWrites_.empty());
    assert(finalSnapshot->writes_.size() == 3);
}

void testRunEventStreamYieldsBeforeCompletion()
{
    std::mutex mutex;
    std::condition_variable cv;
    bool releaseNode = false;
    bool nodeWaiting = false;

    lc::StateGraph graph;
    assert(graph.addNode("node", [&](const lc::State&, lc::Runtime&) {
        std::unique_lock lock(mutex);
        nodeWaiting = true;
        cv.notify_all();
        cv.wait(lock, [&] { return releaseNode; });
        return lc::StateUpdate::fromJson(R"({"done":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "node").isOk());
    assert(graph.addEdge("node", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto streamResult = compiled->streamEvents(stateFromJson("{}"), {}, lc::RunStreamOptions { .capacity_ = 16 });
    assert(streamResult.isOk());
    auto stream = std::move(streamResult).value();

    bool sawNodeStarted = false;
    for (;;) {
        auto event = stream.nextFor(std::chrono::seconds(1));
        assert(event.isOk());
        assert(event->has_value());
        if ((*event)->type_ == lc::RuntimeEventType::NodeStarted && (*event)->node_ == "node") {
            sawNodeStarted = true;
            break;
        }
    }

    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(1), [&] { return nodeWaiting; }));
        assert(sawNodeStarted);
        releaseNode = true;
    }
    cv.notify_all();

    bool sawCompleted = false;
    for (;;) {
        auto event = stream.nextFor(std::chrono::seconds(1));
        assert(event.isOk());
        if (!event->has_value())
            break;
        sawCompleted = sawCompleted || (*event)->type_ == lc::RuntimeEventType::RunCompleted;
    }

    auto final = stream.result();
    assert(final.isOk());
    assert(final->state_.view().at("done") == true);
    assert(sawCompleted);
}

void testCustomReducerAndStateSchemas()
{
    lc::StateGraph graph;
    graph.setInputSchema(lc::JsonSchema::object().property("sum", lc::JsonSchema::integer(), true));
    graph.setStateSchema(
        lc::JsonSchema::object()
            .property("sum", lc::JsonSchema::integer())
            .property("done", lc::JsonSchema::boolean()));
    graph.setOutputSchema(lc::JsonSchema::object().property("done", lc::JsonSchema::boolean(), true));
    assert(graph.addNode("add", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"sum":2,"done":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "add").isOk());
    assert(graph.addEdge("add", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.reducers_.set("sum", [](const nlohmann::json& current, const nlohmann::json& update) -> lc::Result<nlohmann::json> {
        const int lhs = current.is_null() ? 0 : current.get<int>();
        return nlohmann::json(lhs + update.get<int>());
    });

    auto result = compiled->invoke(stateFromJson(R"({"sum":1})"), options);
    assert(result.isOk());
    assert(result->state_.view().at("sum") == 3);
    assert(result->state_.view().at("done") == true);

    auto invalid = compiled->invoke(stateFromJson(R"({"sum":"bad"})"), options);
    assert(!invalid.isOk());
    assert(invalid.status().code() == lc::StatusCode::InvalidArgument);
}

void testRuntimeStoreAvailableToNodes()
{
    auto store = std::make_shared<lc::InMemoryStore>();

    lc::StateGraph graph;
    assert(graph.addNode("remember", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto store = context.store();
        if (!store)
            return lc::Status::failedPrecondition("store missing");
        if (auto stored = store->put({ "agents", std::string(context.executionInfo().threadId_) }, "profile", { { "name", "edge" } }); !stored.isOk())
            return stored.status();
        auto item = store->get({ "agents", std::string(context.executionInfo().threadId_) }, "profile");
        if (!item.isOk())
            return item.status();
        if (!item->has_value())
            return lc::Status::notFound("store item missing");
        return lc::StateUpdate::fromJsonValue({
            { "remembered", (*item)->value_.at("name") },
        });
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "remember").isOk());
    assert(graph.addEdge("remember", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.threadId_ = "store-thread";
    options.store_ = store;

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->state_.view().at("remembered") == "edge");

    auto listed = store->search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "agents" },
    });
    assert(listed.isOk());
    assert(listed->size() == 1);
    assert(listed->front().key_ == "profile");
}

void testNodeRetryTimeoutAndErrorHandler()
{
    std::atomic<int> attempts { 0 };
    std::atomic<int> timeoutRuns { 0 };

    lc::NodeOptions retryOptions;
    retryOptions.retry_.maxAttempts_ = 3;

    lc::NodeOptions timeoutOptions;
    timeoutOptions.timeout_ = std::chrono::milliseconds(1);
    timeoutOptions.errorHandler_ = [](const lc::Status& status, const lc::State&, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
        assert(status.code() == lc::StatusCode::DeadlineExceeded);
        return lc::NodeOutput::update(updateFromJson(R"({"timed_out":true})"));
    };

    lc::StateGraph graph;
    assert(graph.addNode("retry", [&](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        attempts.fetch_add(1, std::memory_order_acq_rel);
        if (context.executionInfo().nodeAttempt_ < 3)
            return lc::Status::unavailable("try again");
        return lc::StateUpdate::fromJsonValue({
            { "attempt", context.executionInfo().nodeAttempt_ },
        });
    }, retryOptions).isOk());
    assert(graph.addNode("timeout", [&](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        timeoutRuns.fetch_add(1, std::memory_order_acq_rel);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return lc::StateUpdate::fromJson(R"({"should_not_win":true})");
    }, timeoutOptions).isOk());
    assert(graph.addEdge(std::string(lc::START), "retry").isOk());
    assert(graph.addEdge("retry", "timeout").isOk());
    assert(graph.addEdge("timeout", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto result = compiled->invoke(stateFromJson("{}"));
    assert(result.isOk());
    assert(attempts.load(std::memory_order_acquire) == 3);
    assert(timeoutRuns.load(std::memory_order_acquire) == 1);
    assert(result->state_.view().at("attempt") == 3);
    assert(result->state_.view().at("timed_out") == true);
    assert(!result->state_.view().contains("should_not_win"));
}

void testMultiInterruptResume()
{
    auto approvalNode = [](std::string id, std::string field) {
        return [id = std::move(id), field = std::move(field)](const lc::State&, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
            if (!context.hasResumeValue()) {
                return lc::NodeOutput::interrupt(lc::Interrupt {
                    .id_ = id,
                    .value_ = {
                        { "field", field },
                    },
                });
            }
            auto update = lc::StateUpdate::fromJsonValue({
                { field, context.resumeValue().at("value") },
            });
            if (!update.isOk())
                return update.status();
            return lc::NodeOutput::update(std::move(*update));
        };
    };

    lc::StateGraph graph;
    graph.setStateSchema(
        lc::JsonSchema::object()
            .property("left_value", lc::JsonSchema::string())
            .property("right_value", lc::JsonSchema::string())
            .property("joined", lc::JsonSchema::boolean())
            .additionalProperties(false));
    assert(graph.addNode("left", approvalNode("left-int", "left_value")).isOk());
    assert(graph.addNode("right", approvalNode("right-int", "right_value")).isOk());
    assert(graph.addNode("join", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        assert(state.view().at("left_value") == "L");
        assert(state.view().at("right_value") == "R");
        return lc::StateUpdate::fromJson(R"({"joined":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "left").isOk());
    assert(graph.addEdge(std::string(lc::START), "right").isOk());
    assert(graph.addEdge("left", "join").isOk());
    assert(graph.addEdge("right", "join").isOk());
    assert(graph.addEdge("join", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "multi-interrupt";
    options.checkpointer_ = checkpointer;

    auto paused = compiled->invoke(stateFromJson("{}"), options);
    assert(paused.isOk());
    assert(paused->status_ == lc::RunStatus::Paused);
    assert(paused->step_ == 1);

    auto latest = latestCheckpoint(*checkpointer, "multi-interrupt");
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->nextNodes_ == std::vector<std::string>({ "left", "right" }));
    assert((*latest)->state_.view().at("__interrupt__").at("interrupts").size() == 2);

    lc::RunOptions resumeOptions;
    resumeOptions.checkpointer_ = checkpointer;
    resumeOptions.command_ = lc::Command::resume({
        { "left-int", { { "value", "L" } } },
        { "right-int", { { "value", "R" } } },
    });

    auto resumed = compiled->resume("multi-interrupt", resumeOptions);
    assert(resumed.isOk());
    assert(resumed->status_ == lc::RunStatus::Completed);
    assert(resumed->state_.view().at("joined") == true);
    assert(!resumed->state_.view().contains("__interrupt__"));
}

void testSubgraphParentCommand()
{
    lc::StateGraph child;
    assert(child.addNode("child_node", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
        return lc::NodeOutput::command(lc::Command::gotoParentNode(
            "after",
            updateFromJson(R"({"child_value":7})")));
    }).isOk());
    assert(child.addEdge(std::string(lc::START), "child_node").isOk());
    auto compiledChild = child.compile();
    assert(compiledChild.isOk());

    lc::StateGraph parent;
    assert(parent.addSubgraph("sub", std::make_shared<lc::CompiledStateGraph>(*compiledChild)).isOk());
    assert(parent.addNode("after", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        assert(state.view().at("child_value") == 7);
        return lc::StateUpdate::fromJson(R"({"after":true})");
    }).isOk());
    assert(parent.addEdge(std::string(lc::START), "sub").isOk());
    assert(parent.addCommandRoute("sub", { "after" }).isOk());
    assert(parent.addEdge("after", std::string(lc::END)).isOk());

    auto compiledParent = parent.compile();
    assert(compiledParent.isOk());

    auto result = compiledParent->invoke(stateFromJson("{}"));
    assert(result.isOk());
    assert(!result->parentCommand_.has_value());
    assert(result->state_.view().at("child_value") == 7);
    assert(result->state_.view().at("after") == true);
}

void testProjectedStreamEmitsParts()
{
    lc::StateGraph graph;
    assert(graph.addNode("node", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto token = lc::RuntimeEvent::create(lc::RuntimeEventType::Token);
        token.payload_ = { { "text", "hello" } };
        if (auto status = context.streamWriter().publish(std::move(token)); !status.isOk())
            return status;
        if (auto status = context.streamWriter().write("progress", { { "pct", 50 } }); !status.isOk())
            return status;
        return lc::StateUpdate::fromJson(R"({"value":1})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "node").isOk());
    assert(graph.addEdge("node", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto streamResult = compiled->streamProjected(
        stateFromJson("{}"),
        {},
        lc::RunProjectionOptions {
            .modes_ = {
                lc::StreamMode::Updates,
                lc::StreamMode::Values,
                lc::StreamMode::Messages,
                lc::StreamMode::Custom,
                lc::StreamMode::Tasks,
                lc::StreamMode::Output,
            },
            .capacity_ = 32,
        });
    assert(streamResult.isOk());
    auto stream = std::move(streamResult).value();

    bool sawUpdate = false;
    bool sawValue = false;
    bool sawMessage = false;
    bool sawCustom = false;
    bool sawTask = false;
    bool sawOutput = false;
    for (;;) {
        auto part = stream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        sawUpdate = sawUpdate || (*part)->mode_ == lc::StreamMode::Updates;
        sawValue = sawValue || (*part)->mode_ == lc::StreamMode::Values;
        sawMessage = sawMessage || (*part)->mode_ == lc::StreamMode::Messages;
        sawCustom = sawCustom || (*part)->mode_ == lc::StreamMode::Custom;
        sawTask = sawTask || (*part)->mode_ == lc::StreamMode::Tasks;
        sawOutput = sawOutput || (*part)->mode_ == lc::StreamMode::Output;
    }

    auto result = stream.result();
    assert(result.isOk());
    assert(result->state_.view().at("value") == 1);
    assert(sawUpdate);
    assert(sawValue);
    assert(sawMessage);
    assert(sawCustom);
    assert(sawTask);
    assert(sawOutput);
}

void testModelNodeStreamingEmitsTokenEvents()
{
    auto model = std::make_shared<lc::FakeChatModel>(std::vector<lc::BaseMessage> {
        lc::BaseMessage::ai("streamed answer"),
    });

    lc::StateGraph graph;
    assert(graph.addNode(
        "model",
        lc::makeModelNode(
            model,
            lc::ModelNodeOptions {
                .stream_ = true,
            })).isOk());
    assert(graph.addEdge(std::string(lc::START), "model").isOk());
    assert(graph.addEdge("model", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);

    auto result = compiled->stream(
        stateFromMessages({ lc::BaseMessage::human("hello") }),
        options);
    assert(result.isOk());
    assert(model->calls() == 1);

    bool sawToken = false;
    for (const auto& event : result->events_) {
        if (event.type_ == lc::RuntimeEventType::Token) {
            sawToken = true;
            assert(event.payload_.at("text") == "streamed answer");
            assert(event.payload_.at("metadata").is_object());
        }
    }
    assert(sawToken);

    auto messages = lc::messagesFromStateJson(result->state_.view());
    assert(messages.isOk());
    assert(messages->size() == 2);
    assert(messages->back().content_ == "streamed answer");
}

void testSyncDurabilityPersistsTaskLevelWrites()
{
    lc::StateGraph graph;
    assert(graph.addNode("a", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"visited":["a"]})");
    }).isOk());
    assert(graph.addNode("b", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"visited":["b"]})");
    }).isOk());
    assert(graph.addNode("c", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"visited":["c"]})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "a").isOk());
    assert(graph.addEdge(std::string(lc::START), "b").isOk());
    assert(graph.addEdge(std::string(lc::START), "c").isOk());
    assert(graph.addEdge("a", std::string(lc::END)).isOk());
    assert(graph.addEdge("b", std::string(lc::END)).isOk());
    assert(graph.addEdge("c", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "sync-durability";
    options.checkpointer_ = checkpointer;
    options.durability_ = lc::Durability::Sync;
    options.maxConcurrency_ = 1;
    options.reducers_.set("visited", lc::ReducerKind::Append);

    auto result = compiled->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->state_.view().at("visited") == nlohmann::json::array({ "a", "b", "c" }));

    auto checkpoints = checkpointHistory(*checkpointer, "sync-durability");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 4);
    assert(checkpoints->at(1).metadata_.at("source") == "task_writes");
    assert(checkpoints->at(1).pendingWrites_.size() == 1);
    assert(checkpoints->at(1).pendingWrites_.front().nodeId_ == "a");
    assert(!checkpoints->at(1).pendingWrites_.front().taskId_.empty());
    assert(checkpoints->at(1).nextNodes_ == std::vector<std::string>({ "b", "c" }));
    assert(checkpoints->at(2).metadata_.at("source") == "task_writes");
    assert(checkpoints->at(2).pendingWrites_.size() == 2);
    assert(checkpoints->at(2).nextNodes_ == std::vector<std::string>({ "c" }));
    assert(checkpoints->back().metadata_.at("source") == "completion");
    assert(checkpoints->back().writes_.size() == 3);
}

void testExitDurabilitySkipsIntermediateCheckpoints()
{
    auto compiled = buildCountingGraph();
    auto checkpointer = std::make_shared<lc::InMemorySaver>();

    lc::RunOptions options;
    options.threadId_ = "exit-durability";
    options.checkpointer_ = checkpointer;
    options.durability_ = lc::Durability::Exit;

    auto result = compiled.invoke(stateFromJson(R"({"count":0})"), options);
    assert(result.isOk());
    assert(result->state_.view().at("count") == 3);

    auto checkpoints = checkpointHistory(*checkpointer, "exit-durability");
    assert(checkpoints.isOk());
    assert(checkpoints->size() == 2);
    assert(checkpoints->front().metadata_.at("source") == "initial");
    assert(checkpoints->back().metadata_.at("source") == "completion");
    assert(checkpoints->back().step_ == 3);
}

void testFunctionStyleSequentialInterrupt()
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
    options.threadId_ = "function-interrupt";
    options.checkpointer_ = checkpointer;

    auto firstPause = compiled->invoke(stateFromJson("{}"), options);
    assert(firstPause.isOk());
    assert(firstPause->status_ == lc::RunStatus::Paused);
    assert(firstPause->state_.view().at("__interrupt__").at("interrupts").front().at("id") == "first");

    lc::RunOptions firstResume;
    firstResume.checkpointer_ = checkpointer;
    firstResume.command_ = lc::Command::resume({ { "first", "one" } });
    auto secondPause = compiled->resume("function-interrupt", firstResume);
    assert(secondPause.isOk());
    assert(secondPause->status_ == lc::RunStatus::Paused);
    const auto& secondInterrupt = secondPause->state_.view().at("__interrupt__").at("interrupts").front();
    assert(secondInterrupt.at("id") == "second");
    assert(secondInterrupt.at("resume_values").at("first") == "one");

    lc::RunOptions secondResume;
    secondResume.checkpointer_ = checkpointer;
    secondResume.command_ = lc::Command::resume({ { "second", "two" } });
    auto completed = compiled->resume("function-interrupt", secondResume);
    assert(completed.isOk());
    assert(completed->status_ == lc::RunStatus::Completed);
    assert(completed->state_.view().at("first") == "one");
    assert(completed->state_.view().at("second") == "two");
    assert(!completed->state_.view().contains("__interrupt__"));
}

void testSubgraphCheckpointNamespaceAndPersistence()
{
    lc::StateGraph child;
    assert(child.addNode("child", [](const lc::State&, lc::Runtime& context) {
        return lc::StateUpdate::fromJsonValue({
            { "child_ns", std::string(context.executionInfo().checkpointNamespace_) },
        });
    }).isOk());
    assert(child.addEdge(std::string(lc::START), "child").isOk());
    assert(child.addEdge("child", std::string(lc::END)).isOk());
    auto compiledChild = child.compile();
    assert(compiledChild.isOk());
    auto childPtr = std::make_shared<lc::CompiledStateGraph>(*compiledChild);

    lc::SubgraphOptions subOptions;
    subOptions.persistence_ = lc::SubgraphPersistenceMode::PerThread;
    subOptions.checkpointNamespace_ = "child";

    lc::StateGraph parent;
    assert(parent.addSubgraph("sub", childPtr, subOptions).isOk());
    assert(parent.addEdge(std::string(lc::START), "sub").isOk());
    assert(parent.addEdge("sub", std::string(lc::END)).isOk());
    auto compiledParent = parent.compile();
    assert(compiledParent.isOk());

    auto checkpointer = std::make_shared<lc::InMemorySaver>();
    lc::RunOptions options;
    options.threadId_ = "subgraph-parent";
    options.checkpointNamespace_ = "root";
    options.checkpointer_ = checkpointer;

    auto result = compiledParent->invoke(stateFromJson("{}"), options);
    assert(result.isOk());
    assert(result->state_.view().at("child_ns") == "root|child");

    auto rootNamespaceChildHistory = checkpointHistory(*checkpointer, "subgraph-parent/sub");
    assert(rootNamespaceChildHistory.isOk());
    assert(rootNamespaceChildHistory->empty());

    auto childHistory = checkpointHistory(*checkpointer, "subgraph-parent/sub", "root|child");
    assert(childHistory.isOk());
    assert(childHistory->size() == 2);
    assert(childHistory->front().checkpointNamespace_ == "root|child");
    assert(childHistory->back().checkpointNamespace_ == "root|child");

    lc::SubgraphOptions statelessOptions;
    statelessOptions.persistence_ = lc::SubgraphPersistenceMode::Stateless;
    statelessOptions.checkpointNamespace_ = "stateless";

    lc::StateGraph statelessParent;
    assert(statelessParent.addSubgraph("stateless", childPtr, statelessOptions).isOk());
    assert(statelessParent.addEdge(std::string(lc::START), "stateless").isOk());
    assert(statelessParent.addEdge("stateless", std::string(lc::END)).isOk());
    auto compiledStatelessParent = statelessParent.compile();
    assert(compiledStatelessParent.isOk());

    lc::RunOptions statelessRun;
    statelessRun.threadId_ = "subgraph-stateless-parent";
    statelessRun.checkpointer_ = checkpointer;
    auto statelessResult = compiledStatelessParent->invoke(stateFromJson("{}"), statelessRun);
    assert(statelessResult.isOk());
    auto statelessChildHistory = checkpointHistory(*checkpointer, "subgraph-stateless-parent/stateless");
    assert(statelessChildHistory.isOk());
    assert(statelessChildHistory->empty());
}

void testStreamResultDrainsBufferedEvents()
{
    lc::StateGraph graph;
    assert(graph.addNode("noisy", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        for (int i = 0; i < 8; ++i) {
            auto status = context.streamWriter().write("progress", { { "index", i } });
            if (!status.isOk())
                return status;
        }
        return lc::StateUpdate::fromJson(R"({"done":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "noisy").isOk());
    assert(graph.addEdge("noisy", std::string(lc::END)).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());

    auto eventStreamResult = compiled->streamEvents(
        stateFromJson("{}"),
        {},
        lc::RunStreamOptions {
            .capacity_ = 1,
        });
    assert(eventStreamResult.isOk());
    auto eventStream = std::move(*eventStreamResult);
    auto eventResultFuture = std::async(std::launch::async, [&eventStream] {
        return eventStream.result();
    });
    if (eventResultFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        eventStream.close();
        assert(false && "RunEventStream::result blocked while buffered events were unread");
    }
    auto eventResult = eventResultFuture.get();
    assert(eventResult.isOk());
    assert(eventResult->state_.view().at("done") == true);

    auto partStreamResult = compiled->streamProjected(
        stateFromJson("{}"),
        {},
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Events },
            .capacity_ = 1,
        });
    assert(partStreamResult.isOk());
    auto partStream = std::move(*partStreamResult);
    auto partResultFuture = std::async(std::launch::async, [&partStream] {
        return partStream.result();
    });
    if (partResultFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        partStream.close();
        assert(false && "RunPartStream::result blocked while buffered parts were unread");
    }
    auto partResult = partResultFuture.get();
    assert(partResult.isOk());
    assert(partResult->state_.view().at("done") == true);
}

void testStreamV3ProjectionNamespaceOutputKeysAndSubgraphs()
{
    lc::StateGraph child;
    assert(child.addNode("child", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        if (auto status = context.streamWriter().write("child-progress", { { "pct", 25 } }); !status.isOk())
            return status;
        return lc::StateUpdate::fromJson(R"({"keep":1,"drop":2})");
    }).isOk());
    assert(child.addEdge(std::string(lc::START), "child").isOk());
    assert(child.addEdge("child", std::string(lc::END)).isOk());
    auto compiledChild = child.compile();
    assert(compiledChild.isOk());

    lc::StateGraph parent;
    assert(parent.addSubgraph("sub", std::make_shared<lc::CompiledStateGraph>(*compiledChild)).isOk());
    assert(parent.addEdge(std::string(lc::START), "sub").isOk());
    assert(parent.addEdge("sub", std::string(lc::END)).isOk());
    auto compiledParent = parent.compile();
    assert(compiledParent.isOk());

    auto withSubgraphs = compiledParent->streamProjected(
        stateFromJson("{}"),
        {},
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Custom, lc::StreamMode::Output },
            .capacity_ = 32,
            .outputKeys_ = { "keep" },
            .includeSubgraphs_ = true,
        });
    assert(withSubgraphs.isOk());
    auto stream = std::move(withSubgraphs).value();

    bool sawChildNamespace = false;
    bool sawFilteredOutput = false;
    for (;;) {
        auto part = stream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        if ((*part)->mode_ == lc::StreamMode::Custom && (*part)->name_ == "child-progress") {
            sawChildNamespace = true;
            assert(!(*part)->ns_.empty());
        }
        if ((*part)->mode_ == lc::StreamMode::Output) {
            sawFilteredOutput = true;
            assert((*part)->data_.contains("keep"));
            assert(!(*part)->data_.contains("drop"));
        }
    }
    auto firstResult = stream.result();
    assert(firstResult.isOk());
    assert(sawChildNamespace);
    assert(sawFilteredOutput);

    auto withoutSubgraphs = compiledParent->streamProjected(
        stateFromJson("{}"),
        {},
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Custom, lc::StreamMode::Output },
            .capacity_ = 32,
            .outputKeys_ = { "keep" },
            .includeSubgraphs_ = false,
        });
    assert(withoutSubgraphs.isOk());
    auto filteredStream = std::move(withoutSubgraphs).value();
    bool sawChildCustom = false;
    bool sawOutput = false;
    for (;;) {
        auto part = filteredStream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        sawChildCustom = sawChildCustom || ((*part)->mode_ == lc::StreamMode::Custom);
        if ((*part)->mode_ == lc::StreamMode::Output) {
            sawOutput = true;
            assert((*part)->data_.contains("keep"));
            assert(!(*part)->data_.contains("drop"));
        }
    }
    auto filteredResult = filteredStream.result();
    assert(filteredResult.isOk());
    assert(!sawChildCustom);
    assert(sawOutput);

    lc::RunOptions protocolRun;
    protocolRun.threadId_ = "stream-protocol";
    protocolRun.checkpointNamespace_ = "root";
    auto protocolParts = compiledParent->streamProjected(
        stateFromJson("{}"),
        protocolRun,
        lc::RunProjectionOptions {
            .modes_ = { lc::StreamMode::Events },
            .capacity_ = 64,
            .includeSubgraphs_ = true,
            .langGraphProtocol_ = true,
        });
    assert(protocolParts.isOk());
    auto protocolStream = std::move(protocolParts).value();
    bool sawEnvelope = false;
    bool sawRootMetadata = false;
    bool sawChildParent = false;
    for (;;) {
        auto part = protocolStream.nextFor(std::chrono::seconds(1));
        assert(part.isOk());
        if (!part->has_value())
            break;
        if ((*part)->mode_ != lc::StreamMode::Events)
            continue;
        const auto& data = (*part)->data_;
        assert(data.contains("event"));
        assert(data.contains("run_id"));
        assert(data.contains("parent_ids"));
        assert(data.contains("metadata"));
        assert(data.contains("data"));
        sawEnvelope = true;
        if (data.at("metadata").contains("checkpoint_ns")
            && data.at("metadata").at("checkpoint_ns") == "root") {
            sawRootMetadata = true;
        }
        if (!(*part)->ns_.empty() && data.at("parent_ids").is_array()
            && !data.at("parent_ids").empty()) {
            sawChildParent = true;
            assert(data.at("metadata").at("checkpoint_ns").get<std::string>().starts_with("root|sub"));
            assert(data.at("metadata").at("trace_path").is_array());
            assert(data.at("metadata").at("namespace").is_array());
            assert(data.at("metadata").at("namespace").front() == "root");
            assert(data.at("metadata").at("namespace").at(1).get<std::string>().starts_with("sub"));
        }
    }
    auto protocolResult = protocolStream.result();
    assert(protocolResult.isOk());
    assert(sawEnvelope);
    assert(sawRootMetadata);
    assert(sawChildParent);
}

void testToolReturnedCommandRoutes()
{
    auto registry = std::make_shared<lc::ToolRegistry>();
    auto registered = registry->add(std::make_shared<lc::FunctionTool>(
        lc::ToolSpec {
            .name_ = "route",
            .description_ = "route to finish",
        },
        [](const lc::ToolRequest&, lc::ToolRuntime&) -> lc::Result<lc::ToolResult> {
            return lc::ToolResult::command(
                lc::Command::gotoNode("finish", updateFromJson(R"({"tool_routed":true})")),
                { { "ok", true } });
        }));
    assert(registered.isOk());

    lc::StateGraph graph;
    assert(graph.addNode("tools", lc::ToolNode(registry)).isOk());
    assert(graph.addNode("finish", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        assert(state.view().at("tool_routed") == true);
        assert(state.view().at("messages").size() == 2);
        return lc::StateUpdate::fromJson(R"({"finished":true})");
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "tools").isOk());
    assert(graph.addCommandRoute("tools", { "finish" }).isOk());
    assert(graph.addEdge("finish", std::string(lc::END)).isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());

    lc::RunOptions options;
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);
    auto result = compiled->invoke(
        *lc::State::fromJsonValue({
            { "messages", lc::messagesToJson({
                              lc::BaseMessage::ai(
                                  "",
                                  { lc::ToolCall {
                                      .id_ = "call-1",
                                      .name_ = "route",
                                      .args_ = nlohmann::json::object(),
                                  } }),
                          }) },
        }),
        options);
    assert(result.isOk());
    assert(result->state_.view().at("tool_routed") == true);
    assert(result->state_.view().at("finished") == true);
}

void testStateGraphOrthogonalFacade()
{
    lc::StateGraph child;
    assert(child.addNode("child_node", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"child_done":true})");
    }).isOk());
    assert(child.setConditionalEntryPoint(
        [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeId> {
            return lc::NodeId("child_node");
        },
        { "child_node" }).isOk());
    assert(child.setFinishPoint("child_node").isOk());
    auto compiledChild = child.compile();
    assert(compiledChild.isOk());
    auto childPtr = std::make_shared<lc::CompiledStateGraph>(std::move(*compiledChild));

    lc::StateGraph graph;
    graph.setNodeDefaults(lc::NodeOptions {});
    graph.setSchemas(lc::StateSchemaOptions {
        .inputSchema_ = lc::JsonSchema::object(),
        .stateSchema_ = lc::JsonSchema::object().property("routed", lc::JsonSchema::boolean()),
        .outputSchema_ = lc::JsonSchema::object().property("child_done", lc::JsonSchema::boolean()),
    });

    assert(graph.addNode("entry", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"entered":true})");
    }).isOk());
    assert(graph.addNode("decide", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
        auto update = lc::StateUpdate::fromJson(R"({"routed":true})");
        if (!update.isOk())
            return update.status();
        return lc::NodeOutput::command(lc::Command::gotoNode("child", std::move(*update)));
    }).isOk());
    assert(graph.addSubgraph("child", childPtr).isOk());
    assert(graph.setNodeOptions("entry", lc::NodeOptions {}).isOk());
    assert(graph.setEntryPoint("entry").isOk());
    assert(graph.addConditionalEdges(
        "entry",
        [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeId> {
            return lc::NodeId("decide");
        },
        { "decide" }).isOk());
    assert(graph.addCommandRoute("decide", { "child" }).isOk());
    assert(graph.setFinishPoint("child").isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto result = compiled->invoke(*lc::State::fromJson("{}"));
    assert(result.isOk());
    assert(result->state_.view().at("entered") == true);
    assert(result->state_.view().at("routed") == true);
    assert(result->state_.view().at("child_done") == true);

    lc::StateGraph sendGraph;
    assert(sendGraph.addNode("fan", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(sendGraph.addNode("worker", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return lc::StateUpdate::fromJsonValue({ { "items", nlohmann::json::array({ state.view().at("item") }) } });
    }).isOk());
    assert(sendGraph.setConditionalEntryPoint(
        [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeId> {
            return lc::NodeId("fan");
        },
        { "fan" }).isOk());
    assert(sendGraph.addConditionalEdges(
        "fan",
        [](const lc::State&, lc::Runtime&) -> lc::Result<std::vector<lc::Send>> {
            return std::vector<lc::Send> {
                lc::Send("worker", *lc::State::fromJson(R"({"item":1})")),
                lc::Send("worker", *lc::State::fromJson(R"({"item":2})")),
            };
        },
        { "worker" }).isOk());
    assert(sendGraph.addEdge("worker", std::string(lc::END)).isOk());
    auto compiledSend = sendGraph.compile();
    assert(compiledSend.isOk());

    lc::RunOptions options;
    options.reducers_.set("items", lc::ReducerKind::Append);
    auto sendResult = compiledSend->invoke(*lc::State::fromJson("{}"), options);
    assert(sendResult.isOk());
    assert(sendResult->state_.view().at("items").size() == 2);
}

void testStateGraphRejectsReservedNodeIds()
{
    lc::StateGraph graph;
    lc::NodeHandler noop = [](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return lc::StateUpdate::empty();
    };

    auto startNode = graph.addNode(std::string(lc::START), noop);
    assert(!startNode.isOk());
    assert(startNode.status().code() == lc::StatusCode::InvalidArgument);

    auto endNode = graph.addNode(std::string(lc::END), noop);
    assert(!endNode.isOk());
    assert(endNode.status().code() == lc::StatusCode::InvalidArgument);

    auto namespaceNode = graph.addNode("parent|child", noop);
    assert(!namespaceNode.isOk());
    assert(namespaceNode.status().code() == lc::StatusCode::InvalidArgument);

    auto namespaceTaskNode = graph.addNode("parent:child", noop);
    assert(!namespaceTaskNode.isOk());
    assert(namespaceTaskNode.status().code() == lc::StatusCode::InvalidArgument);

    assert(graph.addNode("valid", noop).isOk());
    auto startTarget = graph.addEdge("valid", std::string(lc::START));
    assert(!startTarget.isOk());
    assert(startTarget.status().code() == lc::StatusCode::InvalidArgument);
}

void testStateGraphBatchBuildersAreAtomicOnFailure()
{
    lc::StateGraph graph;
    lc::NodeHandler noop = [](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return lc::StateUpdate::empty();
    };

    assert(graph.addNode("a", noop).isOk());
    assert(graph.addNode("b", noop).isOk());
    auto failedEdges = graph.addEdge(std::vector<lc::NodeId> { "a", "" }, "b");
    assert(!failedEdges.isOk());
    assert(failedEdges.status().code() == lc::StatusCode::InvalidArgument);
    assert(graph.addEdge("a", "b").isOk());

    assert(graph.addNode("existing", noop).isOk());
    std::vector<std::pair<lc::NodeId, lc::NodeHandler>> sequence;
    sequence.emplace_back("seq_first", noop);
    sequence.emplace_back("existing", noop);
    auto failedSequence = graph.addSequence(std::move(sequence));
    assert(!failedSequence.isOk());
    assert(failedSequence.status().code() == lc::StatusCode::AlreadyExists);
    assert(graph.addNode("seq_first", noop).isOk());
}

void testOpenRoutesStillValidateGraphReachability()
{
    lc::StateGraph openRouter;
    assert(openRouter.addNode("route", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(openRouter.addNode("target", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"target_ran":true})");
    }).isOk());
    assert(openRouter.addEdge(std::string(lc::START), "route").isOk());
    assert(openRouter.addConditionalEdges(
        "route",
        [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeId> {
            return std::string("target");
        }).isOk());
    assert(openRouter.addEdge("target", std::string(lc::END)).isOk());

    auto compiledOpenRouter = openRouter.compile();
    assert(compiledOpenRouter.isOk());
    auto openRouterResult = compiledOpenRouter->invoke(stateFromJson("{}"));
    assert(openRouterResult.isOk());
    assert(openRouterResult->state_.view().at("target_ran") == true);

    lc::NodeHandler noop = [](const lc::State&, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        return lc::StateUpdate::empty();
    };

    lc::StateGraph unreachableRouter;
    assert(unreachableRouter.addNode("entry", noop).isOk());
    assert(unreachableRouter.addNode("orphan", noop).isOk());
    assert(unreachableRouter.addEdge(std::string(lc::START), "entry").isOk());
    assert(unreachableRouter.addEdge("entry", std::string(lc::END)).isOk());
    assert(unreachableRouter.addConditionalEdges(
        "orphan",
        [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeId> {
            return std::string(lc::END);
        }).isOk());
    auto routerCompile = unreachableRouter.compile();
    assert(!routerCompile.isOk());
    assert(routerCompile.status().code() == lc::StatusCode::FailedPrecondition);

    lc::StateGraph unreachableCommand;
    assert(unreachableCommand.addNode("entry", noop).isOk());
    assert(unreachableCommand.addNode("orphan", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
        return lc::NodeOutput::command(lc::Command::gotoNode(std::string(lc::END)));
    }).isOk());
    assert(unreachableCommand.addEdge(std::string(lc::START), "entry").isOk());
    assert(unreachableCommand.addEdge("entry", std::string(lc::END)).isOk());
    assert(unreachableCommand.addCommandRoute("orphan").isOk());
    auto commandCompile = unreachableCommand.compile();
    assert(!commandCompile.isOk());
    assert(commandCompile.status().code() == lc::StatusCode::FailedPrecondition);
}

void testCompileErrors()
{
    lc::StateGraph graph;
    assert(graph.addNode("node", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::empty();
    }).isOk());
    assert(graph.addEdge(std::string(lc::START), "missing").isOk());

    auto compiled = graph.compile();
    assert(!compiled.isOk());
    assert(compiled.status().code() == lc::StatusCode::FailedPrecondition);
}

} // namespace

int main()
{
    testReducers();
    testRunControl();
    testMinimalGraphCheckpointAndEvents();
    testConditionalLoop();
    testConditionalRouterFanoutAndFanIn();
    testConditionalRouterRejectsUndeclaredFanoutTarget();
    testSendFanoutUsesBranchStateAndFanIn();
    testResumeAfterSendCheckpoint();
    testCommandUpdateGoto();
    testCommandRejectsUndeclaredGoto();
    testMaxConcurrencyCapsParallelNodes();
    testCustomExecutorRunsParallelNodes();
    testParallelFanoutAndFanIn();
    testResumeAfterFanoutCheckpoint();
    testStorageSaverAndResume();
    testCheckpointNamespaceIsQueryDimension();
#if LANGGRAPH_CPP_WITH_SQLITE
    testSQLiteStorageSaverResume();
    testSQLiteCheckpointNamespaceResumeAfterReopen();
    testSQLiteSyncDurabilityPendingWritesReplayAfterReopen();
#endif
    testInterruptAndCommandResume();
    testStreamFilteringAndWriter();
    testStateSnapshotReplayAndUpdateStateFork();
    testPendingWritesSkipCompletedParallelNodesOnResume();
    testRunEventStreamYieldsBeforeCompletion();
    testCustomReducerAndStateSchemas();
    testRuntimeStoreAvailableToNodes();
    testNodeRetryTimeoutAndErrorHandler();
    testMultiInterruptResume();
    testSubgraphParentCommand();
    testProjectedStreamEmitsParts();
    testModelNodeStreamingEmitsTokenEvents();
    testSyncDurabilityPersistsTaskLevelWrites();
    testExitDurabilitySkipsIntermediateCheckpoints();
    testFunctionStyleSequentialInterrupt();
    testSubgraphCheckpointNamespaceAndPersistence();
    testStreamResultDrainsBufferedEvents();
    testStreamV3ProjectionNamespaceOutputKeysAndSubgraphs();
    testToolReturnedCommandRoutes();
    testStateGraphOrthogonalFacade();
    testStateGraphRejectsReservedNodeIds();
    testStateGraphBatchBuildersAreAtomicOnFailure();
    testOpenRoutesStillValidateGraphReachability();
    testMaxSteps();
    testCompileErrors();
    return 0;
}
