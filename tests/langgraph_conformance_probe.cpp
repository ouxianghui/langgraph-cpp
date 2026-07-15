#include "foundation/versioning/versioning.hpp"
#include "langgraph/graph/state_graph.hpp"
#include "langgraph/message/message.hpp"
#include "langgraph/model/chat_model.hpp"
#include "langgraph/state/state_update.hpp"
#include "langgraph/tool/tool.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

template <typename T>
[[nodiscard]] T unwrap(lgc::Result<T> result, std::string_view context)
{
    if (!result.isOk()) {
        std::string message(context);
        message.append(": ");
        message.append(result.status().toString());
        throw std::runtime_error(std::move(message));
    }
    return std::move(result).value();
}

void unwrap(lgc::Result<void> result, std::string_view context)
{
    if (!result.isOk()) {
        std::string message(context);
        message.append(": ");
        message.append(result.status().toString());
        throw std::runtime_error(std::move(message));
    }
}

[[nodiscard]] lgc::State stateFromJson(std::string text)
{
    return unwrap(lgc::State::fromJson(std::move(text)), "parse state");
}

[[nodiscard]] lgc::StateUpdate updateFromJson(std::string text)
{
    return unwrap(lgc::StateUpdate::fromJson(std::move(text)), "parse update");
}

[[nodiscard]] lgc::State stateFromMessages(std::vector<lgc::BaseMessage> messages)
{
    return unwrap(lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson(messages) },
    }), "build message state");
}

[[nodiscard]] std::string runStatusName(lgc::RunStatus status)
{
    switch (status) {
    case lgc::RunStatus::Completed:
        return "completed";
    case lgc::RunStatus::Paused:
        return "paused";
    case lgc::RunStatus::Failed:
        return "failed";
    case lgc::RunStatus::Cancelled:
        return "cancelled";
    case lgc::RunStatus::MaxStepsExceeded:
        return "max_steps_exceeded";
    }
    return "unknown";
}

[[nodiscard]] std::string streamModeName(lgc::StreamMode mode)
{
    switch (mode) {
    case lgc::StreamMode::Events:
        return "events";
    case lgc::StreamMode::Updates:
        return "updates";
    case lgc::StreamMode::Values:
        return "values";
    case lgc::StreamMode::Messages:
        return "messages";
    case lgc::StreamMode::Custom:
        return "custom";
    case lgc::StreamMode::Checkpoints:
        return "checkpoints";
    case lgc::StreamMode::Tasks:
        return "tasks";
    case lgc::StreamMode::Debug:
        return "debug";
    case lgc::StreamMode::Interrupts:
        return "interrupts";
    case lgc::StreamMode::Errors:
        return "errors";
    case lgc::StreamMode::Output:
        return "output";
    }
    return "unknown";
}

[[nodiscard]] nlohmann::json snapshotShape(const lgc::StateSnapshot& snapshot)
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

[[nodiscard]] nlohmann::json runnableConfigScenario()
{
    auto base = unwrap(lgc::RunnableConfig::fromJson({
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
            { "assistant_id", "assistant-a" },
            { "custom", "base" },
        } },
    }), "parse base RunnableConfig");
    auto call = unwrap(lgc::RunnableConfig::fromJson({
        { "tags", nlohmann::json::array({ "call" }) },
        { "metadata", {
            { "tenant", "tenant-b" },
        } },
        { "configurable", {
            { "checkpoint_id", "cp-1" },
            { "task_id", "task-1" },
            { "graph_id", "graph-a" },
            { "custom", "call" },
        } },
        { "custom_top", false },
    }), "parse call RunnableConfig");
    auto merged = unwrap(lgc::mergeRunnableConfigs({ base, call }), "merge RunnableConfig");
    auto patched = unwrap(lgc::patchRunnableConfig(merged, {
        { "callbacks", nlohmann::json::array({ std::string("cb2") }) },
        { "tags", nlohmann::json::array({ "patched" }) },
        { "metadata", {
            { "request_id", "req-1" },
        } },
        { "configurable", {
            { "checkpoint_ns", "patched-root" },
            { "patched", true },
        } },
        { "recursion_limit", 8 },
    }), "patch RunnableConfig");
    auto applied = unwrap(lgc::applyRunnableConfig(lgc::RunOptions {}, merged), "apply RunnableConfig");

    lgc::StateGraph graph;
    unwrap(graph.addNode("node", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"ok":true})");
    }), "add RunnableConfig node");
    unwrap(graph.addEdge(std::string(lgc::START), "node"), "add RunnableConfig start");
    unwrap(graph.addEdge("node", std::string(lgc::END)), "add RunnableConfig end");
    auto compiled = unwrap(graph.compile(), "compile RunnableConfig graph");

    auto stream = unwrap(compiled.streamProjected(
        stateFromJson("{}"),
        applied,
        lgc::RunProjectionOptions {
            .modes_ = { lgc::StreamMode::Events },
            .capacity_ = 16,
            .langGraphProtocol_ = true,
        }), "open RunnableConfig stream");

    nlohmann::json startEnvelope;
    for (;;) {
        auto part = unwrap(stream.nextFor(std::chrono::seconds(1)), "read RunnableConfig stream");
        if (!part.has_value())
            break;
        if (part->mode_ == lgc::StreamMode::Events
            && part->data_.value("event", "") == "on_chain_start") {
            startEnvelope = part->data_;
            break;
        }
    }
    auto result = unwrap(stream.result(), "finish RunnableConfig stream");

    return {
        { "merged", merged.toJson() },
        { "patched", patched.toJson() },
        { "applied", {
            { "thread_id", applied.threadId_ },
            { "checkpoint_ns", applied.checkpointNamespace_ },
            { "run_id", applied.runId_ },
            { "run_name", applied.runName_ },
            { "max_steps", applied.limits_.maxSteps_.has_value()
                    ? nlohmann::json(*applied.limits_.maxSteps_)
                    : nlohmann::json(nullptr) },
            { "max_concurrency", applied.maxConcurrency_ },
            { "tags", applied.tags_ },
            { "metadata", applied.metadata_ },
            { "configurable", applied.configurable_ },
        } },
        { "event_envelope", std::move(startEnvelope) },
        { "output", result.state_.view() },
    };
}

[[nodiscard]] nlohmann::json historySnapshotScenario()
{
    lgc::StateGraph graph;
    unwrap(graph.addNode("tick", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        const auto count = state.view().value("count", 0);
        return lgc::StateUpdate::fromJsonValue({ { "count", count + 1 } });
    }), "add tick");
    unwrap(graph.addEdge(std::string(lgc::START), "tick"), "add start edge");
    unwrap(graph.addEdge("tick", std::string(lgc::END)), "add end edge");
    auto compiled = unwrap(graph.compile(), "compile history graph");

    lgc::RunOptions options;
    options.threadId_ = "conformance-history";
    options.checkpointNamespace_ = "root";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();

    auto result = unwrap(compiled.invoke(stateFromJson(R"({"count":0})"), options), "invoke history graph");
    auto history = unwrap(
        compiled.getStateHistory("conformance-history", options),
        "get history graph state history");

    nlohmann::json snapshots = nlohmann::json::array();
    for (const auto& snapshot : history)
        snapshots.push_back(snapshotShape(snapshot));

    return {
        { "output", result.state_.view() },
        { "history", std::move(snapshots) },
    };
}

[[nodiscard]] nlohmann::json commandGotoScenario()
{
    lgc::StateGraph graph;
    unwrap(graph.addNode("decide", [](const lgc::State&, lgc::Runtime&) -> lgc::Result<lgc::NodeOutput> {
        return lgc::NodeOutput::command(lgc::Command::gotoNode(
            "finish",
            updateFromJson(R"({"routed":true})")));
    }), "add decide");
    unwrap(graph.addNode("finish", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        if (state.view().value("routed", false) != true)
            return lgc::Status::failedPrecondition("command update was not visible to destination node");
        return lgc::StateUpdate::fromJson(R"({"finished":true})");
    }), "add finish");
    unwrap(graph.addEdge(std::string(lgc::START), "decide"), "add command start edge");
    unwrap(graph.addCommandRoute("decide", { "finish" }), "add command destinations");
    unwrap(graph.addEdge("finish", std::string(lgc::END)), "add command end edge");
    auto compiled = unwrap(graph.compile(), "compile command graph");
    auto result = unwrap(compiled.invoke(stateFromJson("{}")), "invoke command graph");
    return {
        { "output", result.state_.view() },
    };
}

[[nodiscard]] nlohmann::json interruptReplayScenario()
{
    lgc::StateGraph graph;
    unwrap(graph.addNode("approve", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
        auto answer = context.interrupt("approval", { { "question", "continue?" } });
        if (!answer.isOk())
            return answer.status();
        return lgc::StateUpdate::fromJsonValue({ { "approved", *answer } });
    }), "add interrupt node");
    unwrap(graph.addEdge(std::string(lgc::START), "approve"), "add interrupt start edge");
    unwrap(graph.addEdge("approve", std::string(lgc::END)), "add interrupt end edge");
    auto compiled = unwrap(graph.compile(), "compile interrupt graph");

    lgc::RunOptions options;
    options.threadId_ = "conformance-interrupt";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();

    auto paused = unwrap(compiled.invoke(stateFromJson("{}"), options), "invoke interrupt graph");
    auto latest = unwrap(compiled.getState("conformance-interrupt", options), "get interrupt state");
    auto replayed = unwrap(compiled.replay("conformance-interrupt", latest.checkpointId_, options), "replay interrupt graph");

    lgc::RunOptions resumeOptions = options;
    resumeOptions.command_ = lgc::Command::resume({ { "approval", true } });
    auto resumed = unwrap(compiled.resume("conformance-interrupt", resumeOptions), "resume interrupt graph");

    return {
        { "paused_status", paused.status_ == lgc::RunStatus::Paused ? "paused" : "other" },
        { "pause_interrupt_id", paused.state_.view().at("__interrupt__").at("interrupts").front().at("id") },
        { "replay_status", replayed.status_ == lgc::RunStatus::Paused ? "paused" : "other" },
        { "replay_interrupt_id", replayed.state_.view().at("__interrupt__").at("interrupts").front().at("id") },
        { "output", resumed.state_.view() },
    };
}

[[nodiscard]] nlohmann::json multiInterruptScenario()
{
    lgc::StateGraph graph;
    auto interruptingNode = [](std::string interruptId, std::string field) {
        return [interruptId = std::move(interruptId), field = std::move(field)](
                   const lgc::State&,
                   lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
            auto answer = context.interrupt(interruptId, { { "field", field } });
            if (!answer.isOk())
                return answer.status();
            return lgc::StateUpdate::fromJsonValue({ { field, *answer } });
        };
    };
    unwrap(graph.addNode("left", interruptingNode("left-int", "left")), "add left interrupt");
    unwrap(graph.addNode("right", interruptingNode("right-int", "right")), "add right interrupt");
    unwrap(graph.addEdge(std::string(lgc::START), "left"), "add left start");
    unwrap(graph.addEdge(std::string(lgc::START), "right"), "add right start");
    unwrap(graph.addEdge("left", std::string(lgc::END)), "add left end");
    unwrap(graph.addEdge("right", std::string(lgc::END)), "add right end");
    auto compiled = unwrap(graph.compile(), "compile multi interrupt graph");

    lgc::RunOptions options;
    options.threadId_ = "conformance-multi-interrupt";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();

    auto paused = unwrap(compiled.invoke(stateFromJson("{}"), options), "invoke multi interrupt graph");
    lgc::RunOptions resumeOptions = options;
    resumeOptions.command_ = lgc::Command::resume({
        { "left-int", "L" },
        { "right-int", "R" },
    });
    auto resumed = unwrap(compiled.resume("conformance-multi-interrupt", resumeOptions), "resume multi interrupt graph");

    return {
        { "paused_status", runStatusName(paused.status_) },
        { "interrupts", paused.state_.view().at("__interrupt__").at("interrupts") },
        { "output", resumed.state_.view() },
    };
}

[[nodiscard]] nlohmann::json sequentialInterruptScenario()
{
    lgc::StateGraph graph;
    unwrap(graph.addNode("ask", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
        auto first = context.interrupt("first", { { "prompt", "first?" } });
        if (!first.isOk())
            return first.status();
        auto second = context.interrupt("second", { { "prompt", "second?" } });
        if (!second.isOk())
            return second.status();
        return lgc::StateUpdate::fromJsonValue({
            { "first", *first },
            { "second", *second },
        });
    }), "add sequential interrupt node");
    unwrap(graph.addEdge(std::string(lgc::START), "ask"), "add sequential start");
    unwrap(graph.addEdge("ask", std::string(lgc::END)), "add sequential end");
    auto compiled = unwrap(graph.compile(), "compile sequential interrupt graph");

    lgc::RunOptions options;
    options.threadId_ = "conformance-sequential-interrupt";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();

    auto firstPause = unwrap(compiled.invoke(stateFromJson("{}"), options), "invoke sequential interrupt graph");
    lgc::RunOptions firstResume = options;
    firstResume.command_ = lgc::Command::resume({ { "first", "one" } });
    auto secondPause = unwrap(compiled.resume("conformance-sequential-interrupt", firstResume), "resume first interrupt");
    auto secondSnapshot = unwrap(compiled.getState("conformance-sequential-interrupt", options), "get second interrupt state");
    auto replayed = unwrap(
        compiled.replay("conformance-sequential-interrupt", secondSnapshot.checkpointId_, options),
        "replay second interrupt");

    lgc::RunOptions secondResume = options;
    secondResume.command_ = lgc::Command::resume({ { "second", "two" } });
    auto completed = unwrap(compiled.resume("conformance-sequential-interrupt", secondResume), "resume second interrupt");

    return {
        { "first_status", runStatusName(firstPause.status_) },
        { "first_interrupt", firstPause.state_.view().at("__interrupt__").at("interrupts").front() },
        { "second_status", runStatusName(secondPause.status_) },
        { "second_interrupt", secondPause.state_.view().at("__interrupt__").at("interrupts").front() },
        { "replay_status", runStatusName(replayed.status_) },
        { "replay_interrupt", replayed.state_.view().at("__interrupt__").at("interrupts").front() },
        { "output", completed.state_.view() },
    };
}

[[nodiscard]] nlohmann::json sendMapReduceScenario()
{
    lgc::StateGraph graph;
    unwrap(graph.addNode("fan", [](const lgc::State&, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        return lgc::StateUpdate::empty();
    }), "add fan");
    unwrap(graph.addNode("worker", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        return lgc::StateUpdate::fromJsonValue({ { "items", nlohmann::json::array({ state.view().at("item") }) } });
    }), "add worker");
    unwrap(graph.addEdge(std::string(lgc::START), "fan"), "add send start edge");
    unwrap(graph.addConditionalEdges(
        "fan",
        [](const lgc::State&, lgc::Runtime&) -> lgc::Result<std::vector<lgc::Send>> {
            return std::vector<lgc::Send> {
                lgc::Send("worker", stateFromJson(R"({"item":1})")),
                lgc::Send("worker", stateFromJson(R"({"item":2})")),
            };
        },
        { "worker" }), "add send router");
    unwrap(graph.addEdge("worker", std::string(lgc::END)), "add send end edge");
    auto compiled = unwrap(graph.compile(), "compile send graph");

    lgc::RunOptions options;
    options.reducers_.set("items", lgc::ReducerKind::Append);
    auto result = unwrap(compiled.invoke(stateFromJson("{}"), options), "invoke send graph");
    return {
        { "output", result.state_.view() },
    };
}

[[nodiscard]] nlohmann::json subgraphBoundaryScenario()
{
    lgc::StateGraph child;
    unwrap(child.addNode("child_node", [](const lgc::State&, lgc::Runtime& context) {
        return lgc::StateUpdate::fromJsonValue({
            { "child", true },
            { "checkpoint_ns", std::string(context.executionInfo().checkpointNamespace_) },
        });
    }), "add child node");
    unwrap(child.addEdge(std::string(lgc::START), "child_node"), "add child start");
    unwrap(child.addEdge("child_node", std::string(lgc::END)), "add child end");
    auto compiledChild = unwrap(child.compile(), "compile child graph");

    lgc::StateGraph parent;
    unwrap(parent.addSubgraph("child", std::make_shared<lgc::CompiledStateGraph>(std::move(compiledChild)), lgc::SubgraphOptions {
        .persistence_ = lgc::SubgraphPersistenceMode::PerThread,
        .checkpointNamespace_ = "child",
    }), "add child subgraph");
    unwrap(parent.addEdge(std::string(lgc::START), "child"), "add parent start");
    unwrap(parent.addEdge("child", std::string(lgc::END)), "add parent end");
    auto compiledParent = unwrap(parent.compile(), "compile parent graph");

    lgc::RunOptions options;
    options.threadId_ = "conformance-subgraph";
    options.checkpointNamespace_ = "root";
    auto checkpointer = std::make_shared<lgc::InMemorySaver>();
    options.checkpointer_ = checkpointer;
    auto result = unwrap(compiledParent.invoke(stateFromJson("{}"), options), "invoke subgraph graph");
    auto childHistory = unwrap(checkpointer->list(lgc::CheckpointListOptions {
        .threadId_ = "conformance-subgraph/child",
        .checkpointNamespace_ = std::string("root|child"),
        .order_ = lgc::CheckpointListOrder::OldestFirst,
    }), "list child subgraph history");
    return {
        { "output", result.state_.view() },
        { "child_history_size", childHistory.size() },
        { "child_thread_id", childHistory.empty() ? "" : childHistory.back().checkpoint_.threadId_ },
        { "child_checkpoint_ns", childHistory.empty() ? "" : childHistory.back().checkpoint_.checkpointNamespace_ },
    };
}

[[nodiscard]] nlohmann::json streamEnvelopeScenario()
{
    auto model = std::make_shared<lgc::FakeChatModel>(
        std::vector<lgc::BaseMessage> { lgc::BaseMessage::ai("hello") });
    lgc::StateGraph graph;
    unwrap(graph.addNode("model", lgc::makeModelNode(model, lgc::ModelNodeOptions {
        .stream_ = true,
    })), "add stream model node");
    unwrap(graph.addEdge(std::string(lgc::START), "model"), "add stream start");
    unwrap(graph.addEdge("model", std::string(lgc::END)), "add stream end");
    auto compiled = unwrap(graph.compile(), "compile stream graph");

    auto input = unwrap(lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson({ lgc::BaseMessage::human("hi") }) },
    }), "build stream input");
    auto stream = unwrap(compiled.streamProjected(
        input,
        {},
        lgc::RunProjectionOptions {
            .modes_ = { lgc::StreamMode::Events },
            .capacity_ = 32,
            .langGraphProtocol_ = true,
        }), "open stream projection");

    nlohmann::json tokenEnvelope;
    for (;;) {
        auto part = unwrap(stream.nextFor(std::chrono::seconds(1)), "read stream part");
        if (!part.has_value())
            break;
        if (part->data_.at("event") == "on_chat_model_stream") {
            tokenEnvelope = part->data_;
            break;
        }
    }
    auto streamResult = unwrap(stream.result(), "finish stream graph");
    (void)streamResult;
    return {
        { "token_envelope", std::move(tokenEnvelope) },
    };
}

[[nodiscard]] nlohmann::json streamProjectionScenario()
{
    auto model = std::make_shared<lgc::FakeChatModel>(
        std::vector<lgc::BaseMessage> { lgc::BaseMessage::ai("hello") });
    lgc::StateGraph graph;
    unwrap(graph.addNode("model", lgc::makeModelNode(model, lgc::ModelNodeOptions {
        .stream_ = true,
    })), "add stream projection model");
    unwrap(graph.addEdge(std::string(lgc::START), "model"), "add stream projection start");
    unwrap(graph.addEdge("model", std::string(lgc::END)), "add stream projection end");
    auto compiled = unwrap(graph.compile(), "compile stream projection graph");

    lgc::RunOptions options;
    options.threadId_ = "conformance-stream-projection";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();
    options.reducers_.set("messages", lgc::ReducerKind::AddMessages);

    auto projected = unwrap(compiled.streamProjected(
        stateFromMessages({ lgc::BaseMessage::human("hi") }),
        options,
        lgc::RunProjectionOptions {
            .modes_ = {
                lgc::StreamMode::Updates,
                lgc::StreamMode::Messages,
                lgc::StreamMode::Tasks,
                lgc::StreamMode::Checkpoints,
                lgc::StreamMode::Output,
            },
            .capacity_ = 64,
            .outputKeys_ = { "messages" },
        }), "open stream projection");

    nlohmann::json modeCounts = nlohmann::json::object();
    nlohmann::json samples = nlohmann::json::object();
    for (;;) {
        auto part = unwrap(projected.nextFor(std::chrono::seconds(1)), "read stream projection");
        if (!part.has_value())
            break;
        const auto mode = streamModeName(part->mode_);
        modeCounts[mode] = modeCounts.value(mode, 0) + 1;
        if (!samples.contains(mode))
            samples[mode] = part->data_;
    }
    auto result = unwrap(projected.result(), "finish stream projection graph");
    return {
        { "status", runStatusName(result.status_) },
        { "mode_counts", std::move(modeCounts) },
        { "samples", std::move(samples) },
    };
}

[[nodiscard]] nlohmann::json streamProjectionV2Scenario()
{
    lgc::StateGraph graph;
    unwrap(graph.addNode("tick", [](const lgc::State& state, lgc::Runtime&) {
        const auto count = state.view().value("count", 0);
        return lgc::StateUpdate::fromJsonValue({ { "count", count + 1 } });
    }), "add stream v2 tick node");
    unwrap(graph.addEdge(std::string(lgc::START), "tick"), "add stream v2 start");
    unwrap(graph.addEdge("tick", std::string(lgc::END)), "add stream v2 end");
    auto compiled = unwrap(graph.compile(), "compile stream v2 graph");

    lgc::RunOptions options;
    options.threadId_ = "conformance-stream-v2";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();

    auto projected = unwrap(compiled.streamProjected(
        stateFromJson(R"({"count":0})"),
        options,
        lgc::RunProjectionOptions {
            .modes_ = { lgc::StreamMode::Updates, lgc::StreamMode::Values },
            .capacity_ = 32,
            .version_ = lgc::StreamProtocolVersion::V2,
        }), "open stream v2 projection");

    nlohmann::json samples = nlohmann::json::object();
    for (;;) {
        auto part = unwrap(projected.nextFor(std::chrono::seconds(1)), "read stream v2 projection");
        if (!part.has_value())
            break;
        const auto mode = streamModeName(part->mode_);
        if (!samples.contains(mode))
            samples[mode] = part->data_;
        if (part->mode_ == lgc::StreamMode::Values
            && part->data_.at("data").value("count", 0) == 1) {
            samples["final_values"] = part->data_;
        }
    }
    auto result = unwrap(projected.result(), "finish stream v2 graph");

    lgc::StateGraph interruptGraph;
    unwrap(interruptGraph.addNode("pause", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
        auto answer = context.interrupt("approval", { { "question", "continue?" } });
        if (!answer.isOk())
            return answer.status();
        return lgc::StateUpdate::fromJsonValue({ { "approved", *answer } });
    }), "add stream v2 interrupt node");
    unwrap(interruptGraph.addEdge(std::string(lgc::START), "pause"), "add stream v2 interrupt start");
    unwrap(interruptGraph.addEdge("pause", std::string(lgc::END)), "add stream v2 interrupt end");
    auto compiledInterrupt = unwrap(interruptGraph.compile(), "compile stream v2 interrupt graph");

    lgc::RunOptions interruptOptions;
    interruptOptions.threadId_ = "conformance-stream-v2-interrupt";
    interruptOptions.checkpointer_ = std::make_shared<lgc::InMemorySaver>();
    auto interrupted = unwrap(compiledInterrupt.streamProjected(
        stateFromJson("{}"),
        interruptOptions,
        lgc::RunProjectionOptions {
            .modes_ = { lgc::StreamMode::Values },
            .capacity_ = 32,
            .version_ = lgc::StreamProtocolVersion::V2,
        }), "open stream v2 interrupt projection");

    nlohmann::json interruptValues;
    for (;;) {
        auto part = unwrap(interrupted.nextFor(std::chrono::seconds(1)), "read stream v2 interrupt projection");
        if (!part.has_value())
            break;
        if (part->mode_ == lgc::StreamMode::Values
            && part->data_.contains("interrupts")
            && !part->data_.at("interrupts").empty()) {
            interruptValues = part->data_;
            break;
        }
    }
    auto interruptedResult = unwrap(interrupted.result(), "finish stream v2 interrupt graph");

    return {
        { "status", runStatusName(result.status_) },
        { "samples", std::move(samples) },
        { "interrupt_status", runStatusName(interruptedResult.status_) },
        { "interrupt_values", std::move(interruptValues) },
    };
}

[[nodiscard]] nlohmann::json streamInterruptErrorScenario()
{
    lgc::StateGraph interruptGraph;
    unwrap(interruptGraph.addNode("pause", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
        auto answer = context.interrupt("approval", { { "question", "approve?" } });
        if (!answer.isOk())
            return answer.status();
        return lgc::StateUpdate::fromJsonValue({ { "approved", *answer } });
    }), "add stream interrupt node");
    unwrap(interruptGraph.addEdge(std::string(lgc::START), "pause"), "add stream interrupt start");
    unwrap(interruptGraph.addEdge("pause", std::string(lgc::END)), "add stream interrupt end");
    auto compiledInterrupt = unwrap(interruptGraph.compile(), "compile stream interrupt graph");

    lgc::RunOptions interruptOptions;
    interruptOptions.threadId_ = "conformance-stream-interrupt";
    interruptOptions.checkpointer_ = std::make_shared<lgc::InMemorySaver>();
    auto interruptStream = unwrap(compiledInterrupt.streamProjected(
        stateFromJson("{}"),
        interruptOptions,
        lgc::RunProjectionOptions {
            .modes_ = { lgc::StreamMode::Events, lgc::StreamMode::Interrupts, lgc::StreamMode::Tasks },
            .capacity_ = 64,
            .langGraphProtocol_ = true,
        }), "open interrupt stream");

    nlohmann::json interruptSamples = nlohmann::json::object();
    for (;;) {
        auto part = unwrap(interruptStream.nextFor(std::chrono::seconds(1)), "read interrupt stream");
        if (!part.has_value())
            break;
        if (part->mode_ == lgc::StreamMode::Interrupts && !interruptSamples.contains("interrupt"))
            interruptSamples["interrupt"] = part->data_;
        if (part->mode_ == lgc::StreamMode::Events
            && part->data_.at("metadata").value("runtime_event_type", "") == "interrupt_requested") {
            interruptSamples["event_envelope"] = part->data_;
        }
        if (part->mode_ == lgc::StreamMode::Tasks
            && !interruptSamples.contains("task")) {
            interruptSamples["task"] = part->data_;
        }
    }
    auto paused = unwrap(interruptStream.result(), "finish interrupt stream");

    lgc::StateGraph errorGraph;
    unwrap(errorGraph.addNode("boom", [](const lgc::State&, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        return lgc::Status::failedPrecondition("boom");
    }), "add error node");
    unwrap(errorGraph.addEdge(std::string(lgc::START), "boom"), "add error start");
    unwrap(errorGraph.addEdge("boom", std::string(lgc::END)), "add error end");
    auto compiledError = unwrap(errorGraph.compile(), "compile error graph");

    lgc::RunOptions errorOptions;
    errorOptions.threadId_ = "conformance-stream-error";
    errorOptions.checkpointer_ = std::make_shared<lgc::InMemorySaver>();
    auto errorStream = unwrap(compiledError.streamProjected(
        stateFromJson("{}"),
        errorOptions,
        lgc::RunProjectionOptions {
            .modes_ = { lgc::StreamMode::Events, lgc::StreamMode::Tasks, lgc::StreamMode::Errors },
            .capacity_ = 64,
            .langGraphProtocol_ = true,
        }), "open error stream");

    nlohmann::json errorSamples = nlohmann::json::object();
    for (;;) {
        auto part = unwrap(errorStream.nextFor(std::chrono::seconds(1)), "read error stream");
        if (!part.has_value())
            break;
        if (part->mode_ == lgc::StreamMode::Events
            && part->data_.at("event") == "on_node_error") {
            errorSamples["node_error_envelope"] = part->data_;
        }
        if (part->mode_ == lgc::StreamMode::Events
            && part->data_.at("event") == "on_chain_error") {
            errorSamples["run_error_envelope"] = part->data_;
        }
        if (part->mode_ == lgc::StreamMode::Tasks
            && part->data_.contains("error")
            && !part->data_.at("error").is_null()) {
            errorSamples["failed_task"] = part->data_;
        }
        if (part->mode_ == lgc::StreamMode::Errors
            && !errorSamples.contains("error_projection")) {
            errorSamples["error_projection"] = part->data_;
        }
    }
    auto failed = errorStream.result();

    return {
        { "interrupt_status", runStatusName(paused.status_) },
        { "interrupt_samples", std::move(interruptSamples) },
        { "error_status", failed.isOk() ? "unexpected_ok" : failed.status().codeName() },
        { "error_samples", std::move(errorSamples) },
    };
}

[[nodiscard]] nlohmann::json toolReturnedCommandScenario()
{
    auto registry = std::make_shared<lgc::ToolRegistry>();
    unwrap(registry->add(std::make_shared<lgc::FunctionTool>(
        lgc::ToolSpec {
            .name_ = "route",
            .description_ = "route to finish",
        },
        [](const lgc::ToolRequest&, lgc::ToolRuntime&) -> lgc::Result<lgc::ToolResult> {
            return lgc::ToolResult::command(
                lgc::Command::gotoNode("finish", updateFromJson(R"({"tool_routed":true})")),
                { { "ok", true } });
        })), "register route tool");

    lgc::StateGraph graph;
    unwrap(graph.addNode("tools", lgc::ToolNode(registry)), "add tools node");
    unwrap(graph.addNode("finish", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"finished":true})");
    }), "add finish node");
    unwrap(graph.addEdge(std::string(lgc::START), "tools"), "add tool start");
    unwrap(graph.addCommandRoute("tools", { "finish" }), "add tool command route");
    unwrap(graph.addEdge("finish", std::string(lgc::END)), "add tool end");
    auto compiled = unwrap(graph.compile(), "compile tool command graph");

    auto input = unwrap(lgc::State::fromJsonValue({
        { "messages", lgc::messagesToJson({
              lgc::BaseMessage::ai(
                  "",
                  { lgc::ToolCall {
                      .id_ = "call-route",
                      .name_ = "route",
                      .args_ = nlohmann::json::object(),
                  } }),
          }) },
    }), "build tool command input");
    lgc::RunOptions options;
    options.reducers_.set("messages", lgc::ReducerKind::AddMessages);
    auto result = unwrap(compiled.invoke(input, options), "invoke tool command graph");
    return {
        { "output", result.state_.view() },
    };
}

[[nodiscard]] nlohmann::json contractScenario()
{
    return {
        { "api_contract_version", lgc::kApiContractVersion },
        { "schema_contract_version", lgc::kSchemaContractVersion },
        { "checkpoint_schema_version", lgc::kCheckpointSchemaVersion },
        { "content_envelope_version", lgc::kContentEnvelopeVersion },
        { "storage_schema_version", lgc::kStorageSchemaVersion },
    };
}

[[nodiscard]] nlohmann::json runProbe()
{
    return {
        { "probe", "langgraph-cpp" },
        { "scenarios", {
            { "history_snapshot", historySnapshotScenario() },
            { "runnable_config", runnableConfigScenario() },
            { "command_goto", commandGotoScenario() },
            { "interrupt_replay", interruptReplayScenario() },
            { "multi_interrupt", multiInterruptScenario() },
            { "sequential_interrupt", sequentialInterruptScenario() },
            { "send_map_reduce", sendMapReduceScenario() },
            { "subgraph_boundary", subgraphBoundaryScenario() },
            { "stream_envelope", streamEnvelopeScenario() },
            { "stream_projection", streamProjectionScenario() },
            { "stream_projection_v2", streamProjectionV2Scenario() },
            { "stream_interrupt_error", streamInterruptErrorScenario() },
            { "tool_returned_command", toolReturnedCommandScenario() },
            { "contract", contractScenario() },
        } },
    };
}

} // namespace

int main()
{
    try {
        std::cout << runProbe().dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "langgraph conformance probe failed: " << error.what() << '\n';
        return 1;
    }
}
