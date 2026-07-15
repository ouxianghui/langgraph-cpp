#include <langgraph_cpp/langgraph.hpp>

#include <chrono>
#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string streamModeName(lgc::StreamMode mode)
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

nlohmann::json partToJson(const lgc::StreamPart& part)
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

    if (part.mode_ == lgc::StreamMode::Events) {
        out["event"] = part.data_.value("event", "");
        out["has_run_id"] = part.data_.contains("run_id");
        out["has_parent_ids"] = part.data_.contains("parent_ids");
        out["has_metadata"] = part.data_.contains("metadata");
    } else if (part.mode_ == lgc::StreamMode::Messages) {
        out["data"] = {
            { "event", part.data_.value("event", "") },
            { "text", part.data_.value("text", "") },
        };
    } else if (part.mode_ == lgc::StreamMode::Checkpoints) {
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
    lgc::StateGraph graph;
    if (auto status = graph.addNode("plan", [](const lgc::State&, lgc::Runtime& context)
            -> lgc::Result<lgc::StateUpdate> {
            auto token = lgc::RuntimeEvent::create(lgc::RuntimeEventType::Token);
            token.payload_ = { { "text", "planning" } };
            if (auto emitted = context.streamWriter().publish(std::move(token)); !emitted.isOk())
                return emitted;
            if (auto written = context.streamWriter().write("progress", { { "phase", "plan" }, { "pct", 50 } });
                !written.isOk())
                return written;
            return lgc::StateUpdate::fromJson(R"({"status":"planned"})");
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    if (auto status = graph.addNode("finish", [](const lgc::State&, lgc::Runtime&) {
            return lgc::StateUpdate::fromJson(R"({"answer":"dispatch maintenance window"})");
        });
        !status.isOk()) {
        std::cerr << status.status() << '\n';
        return 1;
    }

    const auto edgeStatuses = {
        graph.addEdge(std::string(lgc::START), "plan"),
        graph.addEdge("plan", "finish"),
        graph.addEdge("finish", std::string(lgc::END)),
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

    lgc::RunOptions options;
    options.threadId_ = "stream-projection-demo";
    options.checkpointer_ = std::make_shared<lgc::InMemorySaver>();

    auto input = lgc::State::fromJson("{}");
    if (!input.isOk()) {
        std::cerr << input.status() << '\n';
        return 1;
    }

    auto streamResult = compiled->streamProjected(
        *input,
        options,
        lgc::RunProjectionOptions {
            .modes_ = {
                lgc::StreamMode::Events,
                lgc::StreamMode::Updates,
                lgc::StreamMode::Values,
                lgc::StreamMode::Messages,
                lgc::StreamMode::Custom,
                lgc::StreamMode::Tasks,
                lgc::StreamMode::Checkpoints,
                lgc::StreamMode::Output,
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
