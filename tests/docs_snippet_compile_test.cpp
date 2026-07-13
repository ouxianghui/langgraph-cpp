#include <langgraph_cpp/langgraph.hpp>

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void testReadmeMinimalGraphSnippet()
{
    lc::StateGraph graph;
    assert(graph.addNode("hello", [](const lc::State&, lc::Runtime&) {
        return lc::StateUpdate::fromJson(R"({"message":"hello from langgraph-cpp"})");
    }).isOk());
    assert(graph.setEntryPoint("hello").isOk());
    assert(graph.setFinishPoint("hello").isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());
    auto result = compiled->invoke(*input);
    assert(result.isOk());
    assert(result->state_.view().at("message") == "hello from langgraph-cpp");
}

void testApiExamplesStateGraphLoopSnippet()
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
            return json->value("count", 0) >= 3 ? std::string(lc::END) : std::string("tick");
        },
        { "tick", std::string(lc::END) }).isOk());

    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lc::State::fromJson(R"({"count":0})");
    assert(input.isOk());
    auto result = compiled->invoke(*input);
    assert(result.isOk());
    assert(result->state_.view().at("count") == 3);
}

void testStreamProjectionSnippet()
{
    lc::StateGraph graph;
    assert(graph.addNode("answer", [](const lc::State&, lc::Runtime& runtime) -> lc::Result<lc::StateUpdate> {
        if (auto status = runtime.streamWriter().write("docs", { { "phase", "answer" } }); !status.isOk())
            return status;
        return lc::StateUpdate::fromJson(R"({"messages":[{"type":"ai","content":"ok"}],"answer":"ok"})");
    }).isOk());
    assert(graph.setEntryPoint("answer").isOk());
    assert(graph.setFinishPoint("answer").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());

    lc::RunOptions options;
    options.reducers_.set("messages", lc::ReducerKind::AddMessages);
    auto parts = compiled->streamProjected(
        *input,
        options,
        lc::RunProjectionOptions {
            .modes_ = {
                lc::StreamMode::Updates,
                lc::StreamMode::Messages,
                lc::StreamMode::Custom,
                lc::StreamMode::Output,
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
        sawOutput = sawOutput || (*part)->mode_ == lc::StreamMode::Output;
    }
    auto final = stream.result();
    assert(final.isOk());
    assert(sawOutput);
}

void testStoreAndCheckpointSnippet()
{
    lc::StateGraph graph;
    assert(graph.addNode("remember", [](const lc::State&, lc::Runtime& context) -> lc::Result<lc::StateUpdate> {
        auto store = context.store();
        if (!store)
            return lc::Status::failedPrecondition("store is missing");
        if (auto status = store->put(
                { "profile", std::string(context.executionInfo().threadId_) },
                "profile",
                nlohmann::json { { "name", "edge" } });
            !status.isOk()) {
            return status.status();
        }
        return lc::StateUpdate::fromJson(R"({"remembered":true})");
    }).isOk());
    assert(graph.setEntryPoint("remember").isOk());
    assert(graph.setFinishPoint("remember").isOk());
    auto compiled = graph.compile();
    assert(compiled.isOk());
    auto input = lc::State::fromJson("{}");
    assert(input.isOk());

    auto store = std::make_shared<lc::InMemoryStore>();
    auto storage = std::make_shared<lc::MemoryStorage>();
    auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

    lc::RunOptions options;
    options.threadId_ = "docs-thread";
    options.checkpointNamespace_ = "root";
    options.store_ = store;
    options.checkpointer_ = checkpointer;

    auto result = compiled->invoke(*input, options);
    assert(result.isOk());

    auto memories = store->search(lc::StoreSearchOptions {
        .namespacePrefix_ = { "profile" },
        .filter_ = nlohmann::json {
            { "name", "edge" },
        },
    });
    assert(memories.isOk());
    assert(memories->size() == 1);

    auto record = checkpointer->getTuple(lc::CheckpointQuery::latest("docs-thread", "root"));
    assert(record.isOk());
    assert(record->has_value());

    auto page = checkpointer->list(lc::CheckpointListOptions {
        .threadId_ = "docs-thread",
        .checkpointNamespace_ = std::string("root"),
        .limit_ = 10,
    });
    assert(page.isOk());
    assert(!page->empty());
}

void testRunnableConfigSnippet()
{
    auto config = lc::RunnableConfig::fromJson({
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

    auto patched = lc::patchRunnableConfig(
        *config,
        {
            { "tags", { "patched" } },
            { "configurable", {
                { "checkpoint_id", "checkpoint-1" },
            } },
        });
    assert(patched.isOk());

    auto merged = lc::mergeRunnableConfigs({ *config, *patched });
    assert(merged.isOk());

    auto options = lc::applyRunnableConfig(lc::RunOptions {}, *merged);
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
