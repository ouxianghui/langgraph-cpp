# langgraph-cpp 公共 API 示例

本文档用紧凑示例说明当前公共 API 的主要使用方式。接口使用当前 `StateGraph` / `CompiledStateGraph` surface；旧的兼容别名已从公共接口移除。

完整可运行程序见 [../EXAMPLE_MATRIX.md](../EXAMPLE_MATRIX.md)。

## 1. StateGraph 与 CompiledStateGraph

`StateGraph` 用来声明节点和边；`compile()` 会校验图定义，并返回可反复执行的 `CompiledStateGraph`。

```cpp
#include <langgraph_cpp/langgraph.hpp>

lc::StateGraph graph;

graph.addNode("tick", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
    auto json = state.toJson();
    if (!json.isOk())
        return json.status();

    return lc::StateUpdate::fromJsonValue({
        { "count", json->value("count", 0) + 1 },
    });
});

graph.addEdge(std::string(lc::START), "tick");
graph.addConditionalEdges(
    "tick",
    [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        return json->value("count", 0) >= 3 ? std::string(lc::END) : std::string("tick");
    },
    { "tick", std::string(lc::END) });

auto compiled = graph.compile();
auto input = lc::State::fromJson(R"({"count":0})");
auto result = compiled->invoke(*input);
```

条件 router 可以返回多个目标。下一轮 super-step 会并行执行这些目标，并通过配置好的 reducer 确定性合并 update。

```cpp
graph.addConditionalEdges(
    "triage",
    [](const lc::State&, lc::Runtime&) -> lc::Result<std::vector<lc::NodeId>> {
        return std::vector<lc::NodeId> { "temperature", "power" };
    },
    { "temperature", "power" });

options.reducers_.set("checks", lc::ReducerKind::Append);
options.reducers_.set("facts", lc::ReducerKind::MergeObject);
```

如果动态 fan-out 的每个分支需要自己的输入 state，可以从 conditional router 返回 `lc::Send`。每个 Send 分支用 branch-local state 调用目标节点，节点返回的 update 仍会合并回 thread 的图状态。

```cpp
graph.addConditionalEdges(
    "plan",
    [](const lc::State& state, lc::Runtime&) -> lc::Result<std::vector<lc::Send>> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();

        std::vector<lc::Send> sends;
        for (const auto& subject : json->at("subjects")) {
            auto branch = lc::State::fromJsonValue({ { "subject", subject } });
            if (!branch.isOk())
                return branch.status();
            sends.push_back(lc::Send("generate", std::move(*branch)));
        }
        return sends;
    },
    { "generate" });

options.reducers_.set("drafts", lc::ReducerKind::Append);
```

需要在同一个返回值中同时 update state 和选择下一节点时，返回 `Command`。动态目标需要通过 `addCommandRoute()` 声明，方便 `compile()` 校验。

```cpp
graph.addNode("decide", [](const lc::State&, lc::Runtime&) -> lc::Result<lc::NodeOutput> {
    auto update = lc::StateUpdate::fromJson(R"({"decision":"repair"})");
    if (!update.isOk())
        return update.status();
    return lc::NodeOutput::command(lc::Command::gotoNode("repair", std::move(*update)));
});

graph.addCommandRoute("decide", { "repair" });
```

## 2. RunOptions 与流式输出

`RunOptions` 控制 reducer、资源限制、并发、checkpoint、store、事件回调和 resume command。

```cpp
lc::RunOptions options;
options.threadId_ = "thread-1";
options.reducers_.set("messages", lc::ReducerKind::AddMessages);
options.limits_ = lc::ResourceLimits {}.maxSteps(100);
options.maxConcurrency_ = 2;
options.executor_ = lc::makeConcurrentExecutor(2);

auto result = compiled->stream(*input, lc::RunOptions::streamingDefaults());
```

如果调用方需要边运行边消费事件，使用 `streamEvents()` 或 `resumeEvents()`。bounded stream 会在调用方停止读取时施加背压。

```cpp
auto streamResult = compiled->streamEvents(
    *input,
    options,
    lc::RunStreamOptions { .capacity_ = 128 });
if (!streamResult.isOk()) {
    // handle streamResult.status()
}

auto stream = std::move(streamResult).value();
for (;;) {
    auto event = stream.next();
    if (!event.isOk()) {
        // handle event.status()
        break;
    }
    if (!event->has_value())
        break;

    const lc::RuntimeEvent& current = **event;
    // inspect current.type_, current.node_, current.payload_, ...
}

auto final = stream.result();
```

如果需要 LangGraph-style stream modes，使用 `streamProjected()` / `resumeProjected()`。`outputKeys_` 可以把 state-shaped modes 投影到指定字段。

```cpp
auto parts = compiled->streamProjected(
    *input,
    options,
    lc::RunProjectionOptions {
        .modes_ = { lc::StreamMode::Updates, lc::StreamMode::Messages, lc::StreamMode::Errors, lc::StreamMode::Output },
        .capacity_ = 128,
        .outputKeys_ = { "messages" },
    });
```

设置 `langGraphProtocol_` 后，`StreamMode::Events` 会输出带 `event`、`run_id`、`parent_ids`、`metadata` 和 `data` 字段的 LangGraph-style event envelope。

```cpp
auto events = compiled->streamProjected(
    *input,
    options,
    lc::RunProjectionOptions {
        .modes_ = { lc::StreamMode::Events },
        .langGraphProtocol_ = true,
    });
```

## 3. Store、Schema 与节点策略

`RunOptions::store_` 通过 `Runtime::store()` 暴露 namespaced key-value store。

```cpp
auto store = std::make_shared<lc::InMemoryStore>();
options.store_ = store;

graph.addNode("remember", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
    auto store = context.store();
    if (auto status = store->put(
            { "profile", std::string(context.executionInfo().threadId_) },
            "profile",
            nlohmann::json { { "name", "edge" } });
        !status.isOk()) {
        return status.status();
    }
    return lc::StateUpdate::empty();
});

auto memories = store->search(lc::StoreSearchOptions {
    .namespacePrefix_ = { "profile" },
    .filter_ = nlohmann::json {
        { "name", "edge" },
    },
});
```

持久化长期记忆可以用任意 `IStorage` 实现包一层 `StorageStore`。

```cpp
auto storage = std::make_shared<lc::SQLiteStorage>("agent-memory.db");
options.store_ = std::make_shared<lc::StorageStore>(storage);
```

图可以用内置 JSON Schema 子集校验 input/state/output，也可以为字段注册自定义 reducer。

```cpp
graph.setInputSchema(lc::JsonSchema::object().property("count", lc::JsonSchema::integer(), true));
graph.setStateSchema(lc::JsonSchema::object().property("count", lc::JsonSchema::integer()));

options.reducers_.set("count", [](const nlohmann::json& current, const nlohmann::json& update) {
    const int lhs = current.is_null() ? 0 : current.get<int>();
    return lc::Result<nlohmann::json>(nlohmann::json(lhs + update.get<int>()));
});
```

节点策略支持 retry、同步 handler 返回后的 best-effort timeout 检查，以及 fallback error handler。

```cpp
lc::NodeOptions nodeOptions;
nodeOptions.retry_.maxAttempts_ = 3;
nodeOptions.timeout_ = std::chrono::milliseconds(50);
nodeOptions.errorHandler_ = [](const lc::Status&, const lc::State&, lc::Runtime&) {
    return lc::NodeOutput::update(*lc::StateUpdate::fromJsonValue({ { "recovered", true } }));
};

graph.addNode("fragile", fragileHandler, nodeOptions);
```

## 4. Checkpoint Saver

`InMemorySaver` 适合测试和单进程 demo。`StorageSaver` 通过 `IStorage` 实现持久化 checkpoint，例如 `MemoryStorage` 或 `SQLiteStorage`。

```cpp
auto storage = std::make_shared<lc::MemoryStorage>();
auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

lc::RunOptions firstRun;
firstRun.threadId_ = "repair-thread";
firstRun.checkpointNamespace_ = "root";
firstRun.checkpointer_ = checkpointer;
firstRun.limits_ = lc::ResourceLimits {}.maxSteps(2);

auto stopped = compiled->invoke(*input, firstRun);

lc::RunOptions resumeRun;
resumeRun.checkpointNamespace_ = "root";
resumeRun.checkpointer_ = checkpointer;
resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(20);

auto resumed = compiled->resume("repair-thread", resumeRun);
```

SQLite 启用后，同一 checkpoint contract 可以跨进程重启恢复。

```cpp
auto storage = std::make_shared<lc::SQLiteStorage>("agent-checkpoints.db");
auto checkpointer = std::make_shared<lc::StorageSaver>(storage);
```

需要 checkpoint 本体与 task-level pending writes 时，使用 `getTuple()` 和 `list()`。

```cpp
auto record = checkpointer->getTuple(lc::CheckpointQuery::latest("repair-thread", "root"));
if (record.isOk() && record->has_value()) {
    const auto& checkpoint = (*record)->checkpoint_;
    const auto& pendingWrites = (*record)->pendingWrites_;
}

auto page = checkpointer->list(lc::CheckpointListOptions {
    .threadId_ = "repair-thread",
    .checkpointNamespace_ = std::string("root"),
    .limit_ = 10,
    .metadataFilter_ = { { "source", "task_writes" } },
});
```

`AsyncCheckpointSaver` 为核心 saver contract 和维护能力提供 future-returning 变体。

```cpp
lc::AsyncCheckpointSaver async(checkpointer);
auto stored = async.putWrites(lc::CheckpointWriteSet {
    .threadId_ = "repair-thread",
    .checkpointNamespace_ = "root",
    .checkpointId_ = "checkpoint-2",
    .taskId_ = "planner-task",
    .taskPath_ = "planner",
    .writes_ = { /* task-level writes */ },
});

auto pruned = async.prune(
    "repair-thread",
    lc::CheckpointPruneOptions {
        .checkpointNamespace_ = "root",
        .keepLatest_ = 1,
    });
auto cleared = async.deleteThread("repair-thread");
```

`StorageSaverOptions::codec_` 控制持久化 checkpoint serialization。可选实现包括 `JsonCheckpointCodec`、`EnvelopedCheckpointCodec` 和 `SecureCheckpointCodec`。

## 5. History、Replay 与 Update State

checkpointed run 可查询 LangGraph-style state snapshot，并支持 time-travel。

```cpp
auto latest = compiled->getState("repair-thread", resumeRun);
auto history = compiled->getStateHistory("repair-thread", resumeRun);
if (!history.isOk() || history->empty()) {
    // handle history.status() or an empty thread
}

const auto& checkpoint = history->front().checkpointId_;
auto replayed = compiled->replay("repair-thread", checkpoint, resumeRun);

auto patch = lc::StateUpdate::fromJson(R"({"approved":true})");
if (!patch.isOk()) {
    // handle patch.status()
}

lc::StateUpdateOptions updateOptions;
updateOptions.checkpointId_ = checkpoint;
updateOptions.asNode_ = "approve";

auto forked = compiled->updateState(
    "repair-thread",
    std::move(*patch),
    resumeRun,
    updateOptions);
```

## 6. Messages 与 Models

messages 通常作为 state 中的 JSON array 保存，并使用 LangGraph-style `add_messages` reducer。

```cpp
auto input = lc::State::fromJsonValue({
    { "messages", lc::messagesToJson({
        lc::BaseMessage::system("Answer concisely."),
        lc::BaseMessage::human("What is 2 + 3?"),
    }) },
});

auto model = std::make_shared<lc::FakeChatModel>(std::vector<lc::BaseMessage> {
    lc::BaseMessage::ai("5"),
});

lc::StateGraph graph;
graph.addNode("model", lc::makeModelNode(model));
graph.addEdge(std::string(lc::START), "model");
graph.addEdge("model", std::string(lc::END));

lc::RunOptions options;
options.reducers_.set("messages", lc::ReducerKind::AddMessages);
```

开启 model streaming 后，model chunk 会转成 runtime `Token` event，同时最终 assistant message 仍会追加到 state。

```cpp
graph.addNode("model", lc::makeModelNode(
    model,
    lc::ModelNodeOptions {
        .stream_ = true,
    }));
```

可选 llama.cpp adapter 需要 `LANGGRAPH_CPP_WITH_LLAMA_CPP=ON`，并由应用提供 GGUF 模型路径。

```cpp
auto model = std::make_shared<lc::LlamaCppChatModel>(lc::LlamaCppChatModelOptions {
    .modelPath_ = "models/local-model.gguf",
    .contextSize_ = 2048,
    .temperature_ = 0.7F,
    .maxTokens_ = 128,
});

graph.addNode("model", lc::makeModelNode(model));
```

本地 tool calling 可以从注册工具生成受约束 JSON grammar，再让 adapter 把生成的 JSON 解析成 `ToolCall`。

```cpp
auto grammar = lc::toolCallJsonGrammar(*registry);
if (!grammar.isOk()) {
    // handle grammar.status()
}

auto model = std::make_shared<lc::LlamaCppChatModel>(lc::LlamaCppChatModelOptions {
    .modelPath_ = "models/local-model.gguf",
    .contextSize_ = 2048,
    .temperature_ = 0.0F,
    .maxTokens_ = 192,
    .grammar_ = *grammar,
    .parseToolCallJson_ = true,
});
```

## 7. Tools

`ToolRegistry` 持有工具；`ToolExecutor` 负责输入校验、policy、调用、输出校验和 tool-call events。`ToolNode` 从最新 assistant message 读取 tool calls，并把 tool result messages 追加回 state。

```cpp
auto registry = std::make_shared<lc::ToolRegistry>();

registry->add(lc::Tool {
    .name_ = "add",
    .description_ = "Add two integers.",
    .inputSchema_ = lc::JsonSchema::object()
                        .property("a", lc::JsonSchema::integer(), true)
                        .property("b", lc::JsonSchema::integer(), true)
                        .additionalProperties(false),
    .outputSchema_ = lc::JsonSchema::object()
                         .property("value", lc::JsonSchema::integer(), true)
                         .additionalProperties(false),
    .callable_ = [](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
        return nlohmann::json {
            { "value", input.at("a").get<int>() + input.at("b").get<int>() },
        };
    },
});

graph.addNode("tools", lc::ToolNode(
    registry,
    lc::ToolNodeOptions { .validateOutput_ = true }));
```

需要 runtime context 的工具可以注册 `BaseTool` 实现，或使用接收 `ToolRequest` / `ToolRuntime` 的 `FunctionTool`。

```cpp
auto contextTool = std::make_shared<lc::FunctionTool>(
    lc::ToolSpec {
        .name_ = "runtime.echo",
        .description_ = "Echo runtime context.",
        .inputSchema_ = lc::JsonSchema::object()
                            .property("value", lc::JsonSchema::string(), true)
                            .additionalProperties(false),
    },
    [](const lc::ToolRequest& request, lc::ToolRuntime& context) -> lc::Result<lc::ToolResult> {
        return lc::ToolResult::success({
            { "thread_id", context.threadId_ },
            { "value", request.arguments_.at("value") },
        });
    });

registry->add(contextTool);
```

`ToolExecutor` 也可以由应用直接使用，以便控制授权策略。

```cpp
lc::ToolExecutor executor(
    registry,
    lc::ToolPolicy {
        .validateInput_ = true,
        .validateOutput_ = true,
        .authorize_ = [](const lc::ToolSpec&, const lc::ToolRequest&, lc::ToolRuntime&) {
            return lc::okResult();
        },
    });
```

工具执行返回结构化消息：

```json
{"ok":true,"result":{"value":5}}
```

或：

```json
{"ok":false,"error":{"code":"validation_error","message":"..."}}
```

硬件 adapter 目前是 draft interface。真实绑定可以放在 core runtime 外部，并注册成普通工具。

```cpp
class MyGpio final : public lc::IGpioAdapter {
    // Implement configurePin/readPin/writePin using your hardware library.
};

auto gpio = std::make_shared<MyGpio>();
registry->add(lc::Tool {
    .name_ = "edge.gpio_write",
    .description_ = "Write a GPIO line.",
    .inputSchema_ = lc::JsonSchema::object()
                        .property("line", lc::JsonSchema::string(), true)
                        .property("high", lc::JsonSchema::boolean(), true)
                        .additionalProperties(false),
    .callable_ = [gpio](const nlohmann::json& input) -> lc::Result<nlohmann::json> {
        auto status = gpio->writePin(
            input.at("line").get<std::string>(),
            input.at("high").get<bool>() ? lc::GpioLevel::High : lc::GpioLevel::Low);
        if (!status.isOk())
            return status.status();
        return nlohmann::json { { "ok", true } };
    },
});
```

## 8. Interrupt 与 Resume

节点可以通过 `NodeOutput::interrupt()` 暂停图。runtime 会先写入 interrupt checkpoint，再返回 paused run。若同一 super-step 有多个节点同时 interrupt，`Command::resume()` 可以传入按 interrupt id 或 node id keyed 的 JSON object。

```cpp
graph.addNode("approve", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::NodeOutput> {
    if (context.hasResumeValue()) {
        return lc::NodeOutput::update(*lc::StateUpdate::fromJsonValue({
            { "approved", context.resumeValue().value("approved", false) },
        }));
    }

    return lc::NodeOutput::interrupt(lc::Interrupt {
        .id_ = "approval-required",
        .value_ = { { "reason", "tool requires operator approval" } },
    });
});

lc::RunOptions resumeOptions;
resumeOptions.checkpointer_ = checkpointer;
resumeOptions.command_ = lc::Command::resume({ { "approved", true } });

auto resumed = compiled->resume("approval-thread", resumeOptions);
```

## 9. RunnableConfig JSON 桥接

如果需要接收 LangGraph-style JSON config，可以使用 `RunnableConfig` merge/patch/apply helper，把 thread、namespace、recursion limit、concurrency、tags 和 metadata 映射到 `RunOptions`。

```cpp
auto config = lc::RunnableConfig::fromJson({
    { "tags", { "edge", "demo" } },
    { "metadata", { { "device", "bench" } } },
    { "configurable", {
        { "thread_id", "thread-1" },
        { "checkpoint_ns", "root" },
    } },
    { "recursion_limit", 25 },
    { "max_concurrency", 2 },
});

lc::RunOptions options;
if (config.isOk()) {
    auto status = lc::applyRunnableConfig(*config, options);
    if (!status.isOk()) {
        // handle status
    }
}
```

## 10. 关联文档

- [../API_CONTRACT.md](../API_CONTRACT.md)：公共 API 和持久化 schema 合同。
- [../EXAMPLE_MATRIX.md](../EXAMPLE_MATRIX.md)：可运行示例矩阵。
- [../ARCHITECTURE.md](../ARCHITECTURE.md)：模块边界与运行流程。
- [../LIMITATIONS.md](../LIMITATIONS.md)：当前限制和延后能力。
