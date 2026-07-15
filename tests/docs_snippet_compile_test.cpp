#include <langgraph_cpp/langgraph.hpp>

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void testReadmeMinimalGraphSnippet()
{
    lgc::StateGraph graph;
    assert(graph.addNode("hello", [](const lgc::State&, lgc::Runtime&) {
        return lgc::StateUpdate::fromJson(R"({"message":"hello from langgraph-cpp"})");
    }).isOk());
    assert(graph.setEntryPoint("hello").isOk());
    assert(graph.setFinishPoint("hello").isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());
    auto result = compiled->invoke(*input);
    assert(result.isOk());
    assert(result->state_.view().at("message") == "hello from langgraph-cpp");
}

void testApiExamplesStateGraphLoopSnippet()
{
    lgc::StateGraph graph;

    assert(graph.addNode("tick", [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();

        return lgc::StateUpdate::fromJsonValue({
            { "count", json->value("count", 0) + 1 },
        });
    }).isOk());

    assert(graph.addEdge(std::string(lgc::START), "tick").isOk());
    assert(graph.addConditionalEdges(
        "tick",
        [](const lgc::State& state, lgc::Runtime&) -> lgc::Result<lgc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            return json->value("count", 0) >= 3 ? std::string(lgc::END) : std::string("tick");
        },
        { "tick", std::string(lgc::END) }).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lgc::State::fromJson(R"({"count":0})");
    assert(input.isOk());
    auto result = compiled->invoke(*input);
    assert(result.isOk());
    assert(result->state_.view().at("count") == 3);
}

void testStreamProjectionSnippet()
{
    lgc::StateGraph graph;
    assert(graph.addNode("answer", [](const lgc::State&, lgc::Runtime& runtime) -> lgc::Result<lgc::StateUpdate> {
        if (auto status = runtime.streamWriter().write("docs", { { "phase", "answer" } }); !status.isOk())
            return status;
        return lgc::StateUpdate::fromJson(R"({"messages":[{"type":"ai","content":"ok"}],"answer":"ok"})");
    }).isOk());
    assert(graph.setEntryPoint("answer").isOk());
    assert(graph.setFinishPoint("answer").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());

    lgc::RunOptions options;
    options.reducers_.set("messages", lgc::ReducerKind::AddMessages);
    auto parts = compiled->streamProjected(
        *input,
        options,
        lgc::RunProjectionOptions {
            .modes_ = {
                lgc::StreamMode::Updates,
                lgc::StreamMode::Messages,
                lgc::StreamMode::Custom,
                lgc::StreamMode::Output,
            },
            .capacity_ = 128,
            .outputKeys_ = { "messages" },
        });
    assert(parts.isOk());

    auto stream = std::move(*parts);
    bool sawOutput = false;
    for (;;) {
        auto part = stream.next();
        assert(part.isOk());
        if (!part->has_value())
            break;
        sawOutput = sawOutput || (*part)->mode_ == lgc::StreamMode::Output;
    }
    auto final = stream.result();
    assert(final.isOk());
    assert(sawOutput);
}

void testStoreAndCheckpointSnippet()
{
    lgc::StateGraph graph;
    assert(graph.addNode("remember", [](const lgc::State&, lgc::Runtime& context) -> lgc::Result<lgc::StateUpdate> {
        auto store = context.store();
        if (!store)
            return lgc::Status::failedPrecondition("store is missing");
        if (auto status = store->put(
                { "profile", std::string(context.executionInfo().threadId_) },
                "profile",
                nlohmann::json { { "name", "edge" } });
            !status.isOk()) {
            return status.status();
        }
        return lgc::StateUpdate::fromJson(R"({"remembered":true})");
    }).isOk());
    assert(graph.setEntryPoint("remember").isOk());
    assert(graph.setFinishPoint("remember").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lgc::State::fromJson("{}");
    assert(input.isOk());

    auto store = std::make_shared<lgc::InMemoryStore>();
    auto storage = std::make_shared<lgc::MemoryStorage>();
    auto checkpointer = std::make_shared<lgc::StorageSaver>(storage);

    lgc::RunOptions options;
    options.threadId_ = "docs-thread";
    options.checkpointNamespace_ = "root";
    options.store_ = store;
    options.checkpointer_ = checkpointer;

    auto result = compiled->invoke(*input, options);
    assert(result.isOk());

    auto memories = store->search(lgc::StoreSearchOptions {
        .namespacePrefix_ = { "profile" },
        .filter_ = nlohmann::json {
            { "name", "edge" },
        },
    });
    assert(memories.isOk());
    assert(memories->size() == 1);

    auto record = checkpointer->getTuple(lgc::CheckpointQuery::latest("docs-thread", "root"));
    assert(record.isOk());
    assert(record->has_value());

    auto page = checkpointer->list(lgc::CheckpointListOptions {
        .threadId_ = "docs-thread",
        .checkpointNamespace_ = std::string("root"),
        .limit_ = 10,
    });
    assert(page.isOk());
    assert(!page->empty());
}

void testRunnableConfigSnippet()
{
    auto config = lgc::RunnableConfig::fromJson({
        { "tags", { "docs", "snippet" } },
        { "metadata", { { "source", "docs" } } },
        { "run_name", "docs-run" },
        { "configurable", {
            { "thread_id", "docs-thread" },
            { "checkpoint_ns", "root" },
            { "custom_value", 7 },
        } },
    });
    assert(config.isOk());

    auto patched = lgc::patchRunnableConfig(
        *config,
        {
            { "tags", { "patched" } },
            { "configurable", {
                { "checkpoint_id", "checkpoint-1" },
            } },
        });
    assert(patched.isOk());

    auto merged = lgc::mergeRunnableConfigs({ *config, *patched });
    assert(merged.isOk());

    auto options = lgc::applyRunnableConfig(lgc::RunOptions {}, *merged);
    assert(options.isOk());
    assert(options->threadId_ == "docs-thread");
    assert(options->checkpointNamespace_ == "root");
    assert(options->configurable_.at("custom_value") == 7);
}

} // namespace

int main()
{
    testReadmeMinimalGraphSnippet();
    testApiExamplesStateGraphLoopSnippet();
    testStreamProjectionSnippet();
    testStoreAndCheckpointSnippet();
    testRunnableConfigSnippet();
    return 0;
}
