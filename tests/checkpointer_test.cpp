#include <langgraph_cpp/langgraph.hpp>

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

lc::State stateFromJson(const char* text)
{
    auto state = lc::State::fromJson(text);
    assert(state.isOk());
    return *state;
}

lc::CheckpointWrite writeFor(std::string node, const char* updateJson, std::uint64_t order)
{
    return lc::CheckpointWrite {
        .nodeId_ = std::move(node),
        .update_ = stateFromJson(updateJson),
        .order_ = order,
    };
}

lc::Checkpoint checkpoint(
    std::string checkpointId,
    std::string checkpointNamespace,
    std::uint64_t step,
    nlohmann::json metadata,
    std::optional<std::string> parentCheckpointId = std::nullopt)
{
    return lc::Checkpoint {
        .threadId_ = "thread-contract",
        .checkpointId_ = std::move(checkpointId),
        .checkpointNamespace_ = std::move(checkpointNamespace),
        .parentCheckpointId_ = std::move(parentCheckpointId),
        .step_ = step,
        .state_ = stateFromJson(R"({"value":1})"),
        .writes_ = {
            writeFor("seed", R"({"value":1})", 0),
        },
        .channelVersions_ = {
            { "value", step },
        },
        .versionsSeen_ = {
            { "node", { { "value", step } } },
        },
        .updatedChannels_ = { "value" },
        .metadata_ = std::move(metadata),
        .createdAt_ = std::chrono::system_clock::time_point(std::chrono::milliseconds(step)),
    };
}

void exerciseCheckpointer(lc::BaseCheckpointSaver& checkpointer)
{
    assert(checkpointer.put(checkpoint("cp-1", "root", 1, { { "source", "input" } })).isOk());
    assert(checkpointer.put(checkpoint("cp-2", "root", 2, { { "source", "loop" }, { "step", 2 } }, "cp-1")).isOk());
    assert(checkpointer.put(checkpoint("child-1", "root|child", 3, { { "source", "loop" } })).isOk());

    assert(checkpointer.putWrites(lc::CheckpointWriteSet {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = "root",
        .checkpointId_ = "cp-2",
        .taskId_ = "task-a",
        .taskPath_ = "root/a",
        .writes_ = {
            writeFor("a", R"({"a":1})", 0),
            writeFor("b", R"({"b":2})", 1),
        },
    }).isOk());
    assert(checkpointer.putWrites(lc::CheckpointWriteSet {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = "root",
        .checkpointId_ = "cp-2",
        .taskId_ = "task-a",
        .taskPath_ = "root/a",
        .writes_ = {
            writeFor("a", R"({"a":10})", 0),
        },
    }).isOk());

    auto direct = checkpointer.get(lc::CheckpointQuery::at("thread-contract", "cp-2", "root"));
    assert(direct.isOk());
    assert(direct->has_value());
    assert((*direct)->checkpointId_ == "cp-2");
    assert((*direct)->pendingWrites_.size() == 2);
    assert((*direct)->pendingWrites_[0].update_.view().at("a") == 10);

    auto record = checkpointer.getTuple(lc::CheckpointQuery::at("thread-contract", "cp-2", "root"));
    assert(record.isOk());
    assert(record->has_value());
    assert((*record)->checkpoint_.checkpointId_ == "cp-2");
    assert((*record)->checkpoint_.channelVersions_.at("value") == 2);
    assert((*record)->pendingWrites_.size() == 2);
    assert((*record)->pendingWrites_[0].taskId_ == "task-a");
    assert((*record)->pendingWrites_[0].taskPath_ == "root/a");
    assert((*record)->pendingWrites_[0].update_.view().at("a") == 10);
    assert((*record)->pendingWrites_[1].nodeId_ == "b");
    assert((*record)->checkpoint_.pendingWrites_.size() == 2);

    auto latest = checkpointer.getTuple(lc::CheckpointQuery::latest("thread-contract", "root"));
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->checkpoint_.checkpointId_ == "cp-2");
    assert((*latest)->pendingWrites_.size() == 2);

    auto allNamespaces = checkpointer.list(lc::CheckpointListOptions {
        .threadId_ = "thread-contract",
    });
    assert(allNamespaces.isOk());
    assert(allNamespaces->size() == 3);
    assert(allNamespaces->front().checkpoint_.checkpointId_ == "child-1");

    auto rootLimited = checkpointer.list(lc::CheckpointListOptions {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = std::string("root"),
        .limit_ = 1,
    });
    assert(rootLimited.isOk());
    assert(rootLimited->size() == 1);
    assert(rootLimited->front().checkpoint_.checkpointId_ == "cp-2");

    auto olderThanCp2 = checkpointer.list(lc::CheckpointListOptions {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = std::string("root"),
        .beforeCheckpointId_ = std::string("cp-2"),
        .order_ = lc::CheckpointListOrder::OldestFirst,
    });
    assert(olderThanCp2.isOk());
    assert(olderThanCp2->size() == 1);
    assert(olderThanCp2->front().checkpoint_.checkpointId_ == "cp-1");

    auto filtered = checkpointer.list(lc::CheckpointListOptions {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = std::string("root"),
        .metadataFilter_ = { { "source", "loop" } },
    });
    assert(filtered.isOk());
    assert(filtered->size() == 1);
    assert(filtered->front().checkpoint_.checkpointId_ == "cp-2");

    auto delta = checkpointer.getDeltaChannelHistory(lc::DeltaChannelHistoryQuery {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = "root",
        .checkpointId_ = "cp-2",
        .channels_ = { "value" },
    });
    assert(delta.isOk());
    assert(delta->contains("value"));
    assert(delta->at("value").seed_.has_value());
    assert(*delta->at("value").seed_ == 1);
    assert(delta->at("value").writes_.size() == 1);
    assert(delta->at("value").writes_.front().nodeId_ == "seed");

    auto copied = checkpointer.copyThread(lc::CheckpointCopyThreadOptions {
        .sourceThreadId_ = "thread-contract",
        .targetThreadId_ = "thread-copy",
        .checkpointNamespace_ = std::string("root"),
        .targetCheckpointNamespace_ = std::string("copy"),
        .overwriteTarget_ = true,
    });
    assert(copied.isOk());
    assert(copied->remaining_ == 2);
    assert(copied->latestCheckpointId_ == "cp-2");
    auto copiedLatest = checkpointer.getTuple(lc::CheckpointQuery::latest("thread-copy", "copy"));
    assert(copiedLatest.isOk());
    assert(copiedLatest->has_value());
    assert((*copiedLatest)->checkpoint_.threadId_ == "thread-copy");
    assert((*copiedLatest)->checkpoint_.checkpointNamespace_ == "copy");
    assert((*copiedLatest)->pendingWrites_.size() == 2);

    auto prunedCopy = checkpointer.prune(
        "thread-copy",
        lc::CheckpointPruneOptions {
            .checkpointNamespace_ = "copy",
            .keepLatest_ = 1,
        });
    assert(prunedCopy.isOk());
    assert(prunedCopy->removed_ == 1);
    assert(prunedCopy->remaining_ == 1);
    assert(prunedCopy->latestCheckpointId_ == "cp-2");
    assert(checkpointer.put(checkpoint("run-z-step-1", "root", 4, { { "source", "loop" } }, "cp-2")).isOk());
    auto erasedRuns = checkpointer.deleteForRuns(lc::CheckpointDeleteForRunsOptions {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = std::string("root"),
        .runIds_ = { "run-z" },
    });
    assert(erasedRuns.isOk());
    assert(erasedRuns->removed_ == 1);
    auto erasedRunCheckpoint = checkpointer.getTuple(lc::CheckpointQuery::at("thread-contract", "run-z-step-1", "root"));
    assert(erasedRunCheckpoint.isOk());
    assert(!erasedRunCheckpoint->has_value());

    assert(checkpointer.deleteThread("thread-contract").isOk());
    auto deleted = checkpointer.getTuple(lc::CheckpointQuery::latest("thread-contract", "root"));
    assert(deleted.isOk());
    assert(!deleted->has_value());
    auto deletedChild = checkpointer.getTuple(lc::CheckpointQuery::latest("thread-contract", "root|child"));
    assert(deletedChild.isOk());
    assert(!deletedChild->has_value());
    auto emptyHistory = checkpointer.list(lc::CheckpointListOptions {
        .threadId_ = "thread-contract",
    });
    assert(emptyHistory.isOk());
    assert(emptyHistory->empty());
    assert(checkpointer.deleteThread("thread-copy").isOk());
}

void testAsyncCheckpointer()
{
    auto memory = std::make_shared<lc::InMemorySaver>();
    lc::AsyncCheckpointSaver async(memory);

    auto stored = async.put(checkpoint("async-step-1", "root", 1, { { "source", "input" } }));
    assert(stored.ready());
    assert(stored.get().isOk());

    auto writes = async.putWrites(lc::CheckpointWriteSet {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = "root",
        .checkpointId_ = "async-step-1",
        .taskId_ = "async-task",
        .taskPath_ = "async/path",
        .writes_ = {
            writeFor("async-node", R"({"async":true})", 0),
        },
    });
    assert(writes.ready());
    assert(writes.get().isOk());

    auto latest = async.getTuple(lc::CheckpointQuery::latest("thread-contract", "root"));
    assert(latest.ready());
    auto latestResult = latest.get();
    assert(latestResult.isOk());
    assert(latestResult->has_value());
    assert((*latestResult)->checkpoint_.checkpointId_ == "async-step-1");
    assert((*latestResult)->pendingWrites_.size() == 1);

    auto listed = async.list(lc::CheckpointListOptions {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = std::string("root"),
    });
    assert(listed.get().isOk());

    auto delta = async.getDeltaChannelHistory(lc::DeltaChannelHistoryQuery {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = "root",
        .checkpointId_ = "async-step-1",
        .channels_ = { "value" },
    });
    auto deltaResult = delta.get();
    assert(deltaResult.isOk());
    assert(deltaResult->contains("value"));

    assert(async.put(checkpoint("async-run-step-1", "root", 2, { { "source", "loop" } }, "async-step-1")).get().isOk());
    auto deletedRuns = async.deleteForRuns(lc::CheckpointDeleteForRunsOptions {
        .threadId_ = "thread-contract",
        .checkpointNamespace_ = std::string("root"),
        .runIds_ = { "async-run" },
    });
    auto deletedRunsResult = deletedRuns.get();
    assert(deletedRunsResult.isOk());
    assert(deletedRunsResult->removed_ == 1);

    assert(async.copyThread(lc::CheckpointCopyThreadOptions {
        .sourceThreadId_ = "thread-contract",
        .targetThreadId_ = "thread-async-copy",
        .checkpointNamespace_ = std::string("root"),
        .overwriteTarget_ = true,
    }).get().isOk());
    auto prunedCopy = async.prune(
        "thread-async-copy",
        lc::CheckpointPruneOptions {
            .checkpointNamespace_ = "root",
            .keepLatest_ = 1,
        });
    assert(prunedCopy.get().isOk());

    auto cleared = async.deleteThread("thread-contract");
    assert(cleared.get().isOk());
    assert(async.deleteThread("thread-async-copy").get().isOk());
}

} // namespace

int main()
{
    lc::InMemorySaver memory;
    exerciseCheckpointer(memory);

    auto storage = std::make_shared<lc::MemoryStorage>();
    lc::StorageSaver storageSaver(storage);
    exerciseCheckpointer(storageSaver);
    testAsyncCheckpointer();

    lc::InMemorySaver invalid;
    auto badFilter = invalid.list(lc::CheckpointListOptions {
        .threadId_ = "thread-contract",
        .metadataFilter_ = nlohmann::json::array(),
    });
    assert(!badFilter.isOk());
    assert(badFilter.status().code() == lc::StatusCode::InvalidArgument);

    return 0;
}
