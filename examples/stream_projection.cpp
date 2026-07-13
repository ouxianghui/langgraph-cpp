#include <langgraph_cpp/langgraph.hpp>

#include <chrono>
#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string streamModeName(lc::StreamMode mode)
{
    switch (mode) {
    case lc::StreamMode::Events:
        return "events";
    case lc::StreamMode::Updates:
        return "updates";
    case lc::StreamMode::Values:
        return "values";
    case lc::StreamMode::Messages:
        return "messages";
    case lc::StreamMode::Custom:
        return "custom";
    case lc::StreamMode::Checkpoints:
        return "checkpoints";
    case lc::StreamMode::Tasks:
        return "tasks";
    case lc::StreamMode::Debug:
        return "debug";
    case lc::StreamMode::Interrupts:
        return "interrupts";
    case lc::StreamMode::Errors:
        return "errors";
    case lc::StreamMode::Output:
        return "output";
    }
    return "unknown";
}

nlohmann::json partToJson(const lc::StreamPart& part)
{
    const auto mode = streamModeName(part.mode_);
    nlohmann::json out {
        { "mode", mode },
        { "step", part.step_ },
        { "node", part.node_ },
        { "name", part.name_ },
    };
    if (!part.ns_.empty())
        out["namespace"] = part.ns_;

    if (part.mode_ == lc::StreamMode::Events) {
        out["event"] = part.data_.value("event", "");
        out["has_run_id"] = part.data_.contains("run_id");
        out["has_parent_ids"] = part.data_.contains("parent_ids");
        out["has_metadata"] = part.data_.contains("metadata");
    } else if (part.mode_ == lc::StreamMode::Messages) {
        out["data"] = {
            { "event", part.data_.value("event", "") },
            { "text", part.data_.value("text", "") },
        };
    } else if (part.mode_ == lc::StreamMode::Checkpoints) {
        const auto config = part.data_.value("config", nlohmann::json::object());
        const auto configurable = config.value("configurable", nlohmann::json::object());
        out["data"] = {
            { "checkpoint_id", configurable.value("checkpoint_id", "") },
            { "next", part.data_.value("next", nlohmann::json::array()) },
        };
    } else {
        out["data"] = part.data_;
    }
    return out;
}

} // namespace

int main()
{
    lc::StateGraph graph;
    if (auto status = graph.addNode("plan", [](const lc::State&, lc::Runtime& context)
            -> lc::Result<lc::StateUpdate> {
            auto token = lc::RuntimeEvent::create(lc::RuntimeEventType::Token);
            token.payload_ = { { "text", "planning" } };
            if (auto emitted = context.streamWriter().publish(std::move(token)); !emitted.isOk())
                return emitted;
            if (auto written = context.streamWriter().write("progress", { { "phase", "plan" }, { "pct", 50 } });
                !written.isOk())
                return written;
            return lc::StateUpdate::fromJson(R"({"status":"planned"})");
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    if (auto status = graph.addNode("finish", [](const lc::State&, lc::Runtime&) {
            return lc::StateUpdate::fromJson(R"({"answer":"dispatch maintenance window"})");
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    const auto edgeStatuses = {
        graph.addEdge(std::string(lc::START), "plan"),
        graph.addEdge("plan", "finish"),
        graph.addEdge("finish", std::string(lc::END)),
    };
    for (const auto& status : edgeStatuses) {
        if (!status.isOk()) {
            std::cerr << status.status() << '\n';
            return 1;
        }
    }

    auto compiled = graph.compile();
    if (!compiled.isOk()) {
        std::cerr << compiled.status() << '\n';
        return 1;
    }

    lc::RunOptions options;
    options.threadId_ = "stream-projection-demo";
    options.checkpointer_ = std::make_shared<lc::InMemorySaver>();

    auto input = lc::State::fromJson("{}");
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    auto streamResult = compiled->streamProjected(
        *input,
        options,
        lc::RunProjectionOptions {
            .modes_ = {
                lc::StreamMode::Events,
                lc::StreamMode::Updates,
                lc::StreamMode::Values,
                lc::StreamMode::Messages,
                lc::StreamMode::Custom,
                lc::StreamMode::Tasks,
                lc::StreamMode::Checkpoints,
                lc::StreamMode::Output,
            },
            .capacity_ = 64,
            .outputKeys_ = { "answer", "status" },
            .includeSubgraphs_ = true,
            .langGraphProtocol_ = true,
        });
    if (!streamResult.isOk()) {
        std::cerr << streamResult.status() << '\n';
        return 1;
    }

    auto stream = std::move(streamResult).value();
    std::vector<std::string> modesSeen;
    nlohmann::json samples = nlohmann::json::object();
    std::size_t partCount = 0;
    for (;;) {
        auto part = stream.nextFor(std::chrono::seconds(1));
        if (!part.isOk()) {
            std::cerr << part.status() << '\n';
            return 1;
        }
        if (!part->has_value())
            break;

        ++partCount;
        const auto mode = streamModeName((*part)->mode_);
        if (std::find(modesSeen.begin(), modesSeen.end(), mode) == modesSeen.end())
            modesSeen.push_back(mode);
        if (!samples.contains(mode))
            samples[mode] = partToJson(**part);
    }

    auto result = stream.result();
    if (!result.isOk()) {
        std::cerr << result.status() << '\n';
        return 1;
    }

    nlohmann::json output {
        { "part_count", partCount },
        { "modes_seen", modesSeen },
        { "samples", samples },
        { "final_state", result->state_.view() },
    };
    std::cout << output.dump() << '\n';
    return 0;
}
