#include <langgraph_cpp/langgraph.hpp>

#include <foundation/storage/sqlite_storage.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

namespace {

void require(lc::Result<void> result)
{
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        std::exit(1);
    }
}

lc::CompiledStateGraph buildGraph()
{
    lc::StateGraph graph;

    require(graph.addNode("tick", [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::StateUpdate> {
        auto json = state.toJson();
        if (!json.isOk())
            return json.status();
        return lc::StateUpdate::fromJsonValue({
            { "count", json->value("count", 0) + 1 },
        });
    }));

    require(graph.addEdge(std::string(lc::START), "tick"));
    require(graph.addConditionalEdges(
        "tick",
        [](const lc::State& state, lc::Runtime&) -> lc::Result<lc::NodeId> {
            auto json = state.toJson();
            if (!json.isOk())
                return json.status();
            if (json->value("count", 0) >= 4)
                return std::string(lc::END);
            return std::string("tick");
        },
        { "tick", std::string(lc::END) }));

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        std::exit(1);
    }
    return *compiled;
}

std::filesystem::path exampleDatabasePath()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::filesystem::temp_directory_path()
        / ("langgraph_cpp_sqlite_checkpoint_" + std::to_string(millis) + ".db");
}

} // namespace

int main()
{
    const auto dbPath = exampleDatabasePath();
    const auto threadId = std::string("sqlite-checkpoint-demo");
    auto graph = buildGraph();

    {
        auto storage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
        auto checkpointer = std::make_shared<lc::StorageSaver>(storage);

        lc::RunOptions firstRun;
        firstRun.threadId_ = threadId;
        firstRun.checkpointer_ = checkpointer;
        firstRun.limits_ = lc::ResourceLimits {}.maxSteps(2);

        auto input = lc::State::fromJson(R"({"count":0})");
        if (!input.isOk()) {
            std::cerr << input.status() << '\n';
            return 1;
        }

        auto interrupted = graph.invoke(*input, firstRun);
        if (interrupted.isOk()) {
            std::cerr << "expected first run to stop at max steps\n";
            return 1;
        }
    }

    auto reopenedStorage = std::make_shared<lc::SQLiteStorage>(dbPath.string());
    auto reopenedCheckpointer = std::make_shared<lc::StorageSaver>(reopenedStorage);

    lc::RunOptions resumeRun;
    resumeRun.checkpointer_ = reopenedCheckpointer;
    resumeRun.limits_ = lc::ResourceLimits {}.maxSteps(10);

    auto resumed = graph.resume(threadId, resumeRun);
    if (!resumed.isOk()) {
        std::cerr << resumed.status() << '\n';
        return 1;
    }

    auto checkpoints = reopenedCheckpointer->list(lc::CheckpointListOptions {
        .threadId_ = threadId,
        .checkpointNamespace_ = std::string(),
        .order_ = lc::CheckpointListOrder::OldestFirst,
    });
    if (!checkpoints.isOk()) {
        std::cerr << checkpoints.status() << '\n';
        return 1;
    }

    std::cout << nlohmann::json {
        { "database", dbPath.string() },
        { "checkpoint_count", checkpoints->size() },
        { "state", resumed->state_.json() },
        { "status", "completed" },
    }.dump() << '\n';
    return 0;
}
