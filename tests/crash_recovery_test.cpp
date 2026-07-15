#include "foundation/process/process.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/storage/i_storage.hpp"
#include "langgraph/checkpoint/checkpointer.hpp"
#include "langgraph/core/ids.hpp"
#include "langgraph/store/store.hpp"

#if LANGGRAPH_CPP_WITH_SQLITE
#include "foundation/storage/sqlite_storage.hpp"
#endif

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::string hexEncode(std::string_view value)
{
    constexpr std::array<char, 16> digits {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string out;
    out.reserve(value.size() * 2);
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        out.push_back(digits[byte >> 4U]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

[[nodiscard]] lgc::State stateFromJson(std::string text)
{
    auto state = lgc::State::fromJson(std::move(text));
    assert(state.isOk());
    return std::move(*state);
}

[[nodiscard]] lgc::Checkpoint checkpointForThread(
    std::string threadId,
    std::string checkpointNamespace,
    std::string id,
    std::uint64_t step,
    std::string stateJson)
{
    return lgc::Checkpoint {
        .threadId_ = std::move(threadId),
        .checkpointId_ = std::move(id),
        .checkpointNamespace_ = std::move(checkpointNamespace),
        .step_ = step,
        .state_ = stateFromJson(std::move(stateJson)),
        .nextNodes_ = { std::string(lgc::END) },
        .metadata_ = { { "source", "crash-test" } },
        .createdAt_ = std::chrono::system_clock::now() + std::chrono::seconds(step),
    };
}

[[nodiscard]] lgc::Checkpoint checkpoint(
    std::string id,
    std::uint64_t step,
    std::string stateJson)
{
    return checkpointForThread(
        "crash-thread",
        {},
        std::move(id),
        step,
        std::move(stateJson));
}

[[nodiscard]] std::string storageValueForCheckpoint(const lgc::Checkpoint& value)
{
    lgc::JsonCheckpointCodec codec;
    auto payload = codec.encode(value);
    assert(payload.isOk());
    return nlohmann::json {
        { "content_type", payload->contentType_ },
        { "data", payload->data_ },
    }.dump();
}

[[nodiscard]] std::string paddedIndex(std::uint64_t value)
{
    auto text = std::to_string(value);
    if (text.size() >= 20U)
        return text;
    return std::string(20U - text.size(), '0') + text;
}

[[nodiscard]] std::string storageValueForWrite(const lgc::CheckpointWrite& value)
{
    lgc::JsonCheckpointCodec codec;
    auto payload = codec.encodeWrite(value);
    assert(payload.isOk());
    return nlohmann::json {
        { "content_type", payload->contentType_ },
        { "data", payload->data_ },
    }.dump();
}

[[nodiscard]] lgc::StorageKey checkpointKey(
    std::string_view threadId,
    std::string_view checkpointId,
    std::string_view checkpointNamespace = {})
{
    std::string key = hexEncode(threadId);
    key.push_back('/');
    if (!checkpointNamespace.empty()) {
        key.append("ns/");
        key.append(hexEncode(checkpointNamespace));
        key.push_back('/');
    }
    key.append(hexEncode(checkpointId));
    return lgc::StorageKey {
        .scope_ = "langgraph/checkpoints",
        .key_ = std::move(key),
    };
}

[[nodiscard]] lgc::StorageKey latestPointerKey(
    std::string_view threadId,
    std::string_view checkpointNamespace = {})
{
    std::string key = "latest/" + hexEncode(threadId);
    if (!checkpointNamespace.empty()) {
        key.append("/ns/");
        key.append(hexEncode(checkpointNamespace));
    }
    return lgc::StorageKey {
        .scope_ = "langgraph/checkpoints",
        .key_ = std::move(key),
    };
}

[[nodiscard]] lgc::StorageKey checkpointWriteKey(
    std::string_view threadId,
    std::string_view checkpointNamespace,
    std::string_view checkpointId,
    std::string_view taskId,
    std::string_view taskPath,
    std::uint64_t order)
{
    std::string key = "writes/";
    key.append(hexEncode(threadId));
    key.push_back('/');
    if (!checkpointNamespace.empty()) {
        key.append("ns/");
        key.append(hexEncode(checkpointNamespace));
        key.push_back('/');
    }
    key.append("checkpoint/");
    key.append(hexEncode(checkpointId));
    key.push_back('/');
    key.append(hexEncode(taskId));
    key.push_back('/');
    key.append(hexEncode(taskPath));
    key.push_back('/');
    key.append(paddedIndex(order));
    return lgc::StorageKey {
        .scope_ = "langgraph/checkpoints",
        .key_ = std::move(key),
    };
}

[[nodiscard]] lgc::StorageKey storeKey(std::string_view key)
{
    return lgc::StorageKey {
        .scope_ = "langgraph/store",
        .key_ = "items/" + hexEncode("edge") + "/@/" + hexEncode(key),
    };
}

#if LANGGRAPH_CPP_WITH_SQLITE

[[nodiscard]] lgc::SQLiteStorageOptions sqliteOptions(
    lgc::SQLiteJournalMode journalMode = lgc::SQLiteJournalMode::Wal,
    lgc::SQLiteSynchronousMode synchronousMode = lgc::SQLiteSynchronousMode::Full,
    std::chrono::milliseconds busyTimeout = std::chrono::seconds(5))
{
    return lgc::SQLiteStorageOptions {
        .busyTimeout_ = busyTimeout,
        .journalMode_ = journalMode,
        .synchronousMode_ = synchronousMode,
    };
}

[[nodiscard]] std::shared_ptr<lgc::SQLiteStorage> openSQLite(
    const std::string& path,
    lgc::SQLiteStorageOptions options = sqliteOptions())
{
    auto storage = std::make_shared<lgc::SQLiteStorage>(
        path,
        lgc::Logger::defaultLogger(),
        lgc::StorageLimits {},
        lgc::SystemWallClock::instance(),
        std::move(options));
    auto opened = storage->open();
    assert(opened.isOk());
    return storage;
}

[[nodiscard]] lgc::SQLiteJournalMode journalModeFromName(std::string_view name)
{
    if (name == "wal")
        return lgc::SQLiteJournalMode::Wal;
    if (name == "delete")
        return lgc::SQLiteJournalMode::Delete;
    assert(false);
    return lgc::SQLiteJournalMode::Wal;
}

[[nodiscard]] lgc::SQLiteSynchronousMode synchronousModeFromName(std::string_view name)
{
    if (name == "normal")
        return lgc::SQLiteSynchronousMode::Normal;
    if (name == "full")
        return lgc::SQLiteSynchronousMode::Full;
    if (name == "extra")
        return lgc::SQLiteSynchronousMode::Extra;
    assert(false);
    return lgc::SQLiteSynchronousMode::Full;
}

[[nodiscard]] std::string journalModeName(lgc::SQLiteJournalMode mode)
{
    switch (mode) {
    case lgc::SQLiteJournalMode::Wal:
        return "wal";
    case lgc::SQLiteJournalMode::Delete:
        return "delete";
    }
    return "wal";
}

[[nodiscard]] std::string synchronousModeName(lgc::SQLiteSynchronousMode mode)
{
    switch (mode) {
    case lgc::SQLiteSynchronousMode::Normal:
        return "normal";
    case lgc::SQLiteSynchronousMode::Full:
        return "full";
    case lgc::SQLiteSynchronousMode::Extra:
        return "extra";
    }
    return "full";
}

[[nodiscard]] std::vector<std::string_view> splitMode(std::string_view mode)
{
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    for (;;) {
        const auto end = mode.find(':', begin);
        parts.push_back(end == std::string_view::npos
                ? mode.substr(begin)
                : mode.substr(begin, end - begin));
        if (end == std::string_view::npos)
            break;
        begin = end + 1U;
    }
    return parts;
}

int crashChild(const std::string& databasePath, const std::string& mode)
{
    auto modeParts = splitMode(mode);
    auto options = sqliteOptions();
    if (modeParts.size() == 3U
        && (modeParts.front() == "matrix" || modeParts.front() == "kill-loop")) {
        options = sqliteOptions(
            journalModeFromName(modeParts[1]),
            synchronousModeFromName(modeParts[2]));
    }

    auto storage = openSQLite(databasePath, options);
    lgc::StorageSaver checkpointer(storage);
    lgc::StorageStore store(storage);

    if (mode == "committed") {
        assert(checkpointer.put(checkpoint("cp-1", 1, R"({"value":1})")).isOk());
        assert(checkpointer.put(checkpoint("cp-2", 2, R"({"value":2})")).isOk());
        assert(store.put({ "edge" }, "memory", { { "durable", true } }).isOk());
        assert(storage->flush().isOk());
        std::_Exit(86);
    }

    if (modeParts.size() == 3U && modeParts.front() == "matrix") {
        assert(checkpointer.put(checkpoint("cp-1", 1, R"({"value":1})")).isOk());
        assert(checkpointer.put(checkpoint("cp-2", 2, R"({"value":2})")).isOk());
        assert(store.put({ "edge" }, "memory", {
            { "durable", true },
            { "journal", modeParts[1] },
            { "sync", modeParts[2] },
        }).isOk());
        std::_Exit(86);
    }

    if (mode == "partial") {
        assert(checkpointer.put(checkpoint("cp-1", 1, R"({"value":1})")).isOk());
        const auto cp2 = checkpoint("cp-2", 2, R"({"value":2})");
        assert(storage->put(
            checkpointKey(cp2.threadId_, cp2.checkpointId_),
            storageValueForCheckpoint(cp2),
            lgc::StoragePutOptions { .mode_ = lgc::StoragePutMode::InsertOnly })
            .isOk());
        assert(storage->flush().isOk());
        std::_Exit(87);
    }

    if (modeParts.size() == 2U && modeParts.front() == "writer") {
        const std::string threadId = std::string("contention-thread-") + std::string(modeParts[1]);
        for (std::uint64_t step = 1; step <= 8; ++step) {
            assert(checkpointer.put(checkpointForThread(
                threadId,
                {},
                "cp-" + std::to_string(step),
                step,
                "{\"writer\":\"" + std::string(modeParts[1]) + "\",\"value\":" + std::to_string(step) + "}"))
                    .isOk());
        }
        assert(store.put({ "edge", threadId }, "memory", {
            { "writer", modeParts[1] },
            { "durable", true },
        }).isOk());
        assert(storage->flush().isOk());
        return 0;
    }

    if (modeParts.size() == 3U && modeParts.front() == "kill-loop") {
        assert(checkpointer.put(checkpointForThread(
            "kill-thread",
            {},
            "cp-0",
            0,
            R"({"value":0})")).isOk());
        assert(store.put({ "edge" }, "kill-marker", { { "started", true } }).isOk());
        for (std::uint64_t step = 1;; ++step) {
            assert(checkpointer.put(checkpointForThread(
                "kill-thread",
                {},
                "cp-" + std::to_string(step),
                step,
                "{\"value\":" + std::to_string(step) + "}"))
                    .isOk());
            if (step % 16U == 0U)
                assert(storage->flush().isOk());
        }
    }

    return 2;
}

[[nodiscard]] std::filesystem::path uniqueDatabasePath(std::string_view name)
{
    return std::filesystem::temp_directory_path()
        / (std::string("langgraph-cpp-") + std::string(name) + ".sqlite");
}

void removeDatabaseFiles(const std::filesystem::path& databasePath)
{
    std::filesystem::remove(databasePath);
    std::filesystem::remove(databasePath.string() + "-wal");
    std::filesystem::remove(databasePath.string() + "-shm");
    std::filesystem::remove(databasePath.string() + "-journal");
}

void runCrashChild(
    const char* executable,
    const std::filesystem::path& databasePath,
    std::string mode,
    std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    lgc::ProcessRunner runner;
    auto result = runner.run(lgc::ProcessOptions {
        .executable_ = executable,
        .arguments_ = {
            "--crash-child",
            databasePath.string(),
            std::move(mode),
        },
        .timeout_ = timeout,
    });
    assert(result.isOk());
    assert(result->exited_);
    assert(result->exitCode_ == 86 || result->exitCode_ == 87);
}

void runSuccessfulChild(
    const char* executable,
    const std::filesystem::path& databasePath,
    std::string mode)
{
    lgc::ProcessRunner runner;
    auto result = runner.run(lgc::ProcessOptions {
        .executable_ = executable,
        .arguments_ = {
            "--crash-child",
            databasePath.string(),
            std::move(mode),
        },
        .timeout_ = std::chrono::seconds(10),
    });
    assert(result.isOk());
    assert(result->success());
}

void runKilledChild(
    const char* executable,
    const std::filesystem::path& databasePath,
    std::string mode)
{
    lgc::ProcessRunner runner;
    auto result = runner.run(lgc::ProcessOptions {
        .executable_ = executable,
        .arguments_ = {
            "--crash-child",
            databasePath.string(),
            std::move(mode),
        },
        .timeout_ = std::chrono::milliseconds(250),
    });
    assert(result.isOk());
    assert(result->timedOut_);
    assert(!result->status_.isOk());
    assert(result->status_.code() == lgc::StatusCode::DeadlineExceeded);
}

void testReopenAfterHardExit(const char* executable)
{
    const auto path = uniqueDatabasePath("committed");
    removeDatabaseFiles(path);
    runCrashChild(executable, path, "committed");

    auto storage = openSQLite(path.string());
    lgc::StorageSaver checkpointer(storage);
    auto latest = checkpointer.getTuple(lgc::CheckpointQuery::latest("crash-thread"));
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->checkpoint_.checkpointId_ == "cp-2");
    assert((*latest)->checkpoint_.state_.view().at("value") == 2);

    lgc::StorageStore store(storage);
    auto item = store.get({ "edge" }, "memory");
    assert(item.isOk());
    assert(item->has_value());
    assert((*item)->value_.at("durable") == true);
}

void testJournalAndSynchronousHardExitMatrix(const char* executable)
{
    const std::array journals {
        lgc::SQLiteJournalMode::Wal,
        lgc::SQLiteJournalMode::Delete,
    };
    const std::array syncModes {
        lgc::SQLiteSynchronousMode::Normal,
        lgc::SQLiteSynchronousMode::Full,
        lgc::SQLiteSynchronousMode::Extra,
    };

    for (const auto journal : journals) {
        for (const auto sync : syncModes) {
            const auto journalName = journalModeName(journal);
            const auto syncName = synchronousModeName(sync);
            const auto path = uniqueDatabasePath("matrix-" + journalName + "-" + syncName);
            removeDatabaseFiles(path);
            runCrashChild(executable, path, "matrix:" + journalName + ":" + syncName);

            auto storage = openSQLite(path.string(), sqliteOptions(journal, sync));
            lgc::StorageSaver checkpointer(storage);
            auto latest = checkpointer.getTuple(lgc::CheckpointQuery::latest("crash-thread"));
            assert(latest.isOk());
            assert(latest->has_value());
            assert((*latest)->checkpoint_.checkpointId_ == "cp-2");
            assert((*latest)->checkpoint_.state_.view().at("value") == 2);

            lgc::StorageStore store(storage);
            auto item = store.get({ "edge" }, "memory");
            assert(item.isOk());
            assert(item->has_value());
            assert((*item)->value_.at("journal") == journalName);
            assert((*item)->value_.at("sync") == syncName);
        }
    }
}

void testPartialCheckpointWriteReconcilesLatestPointer(const char* executable)
{
    const auto path = uniqueDatabasePath("partial");
    removeDatabaseFiles(path);
    runCrashChild(executable, path, "partial");

    auto storage = openSQLite(path.string());
    lgc::StorageSaver checkpointer(storage);
    auto latest = checkpointer.getTuple(lgc::CheckpointQuery::latest("crash-thread"));
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->checkpoint_.checkpointId_ == "cp-2");
    assert((*latest)->checkpoint_.state_.view().at("value") == 2);

}

void testDanglingAndStaleLatestPointerReconcileByScan()
{
    const auto path = uniqueDatabasePath("pointer-reconcile");
    removeDatabaseFiles(path);
    auto storage = openSQLite(path.string());
    lgc::StorageSaver checkpointer(storage);
    assert(checkpointer.put(checkpoint("cp-1", 1, R"({"value":1})")).isOk());
    assert(checkpointer.put(checkpoint("cp-2", 2, R"({"value":2})")).isOk());

    assert(storage->put(
        latestPointerKey("crash-thread"),
        "missing-cp",
        lgc::StoragePutOptions { .mode_ = lgc::StoragePutMode::Upsert })
            .isOk());
    auto latestAfterDanglingPointer = checkpointer.getTuple(lgc::CheckpointQuery::latest("crash-thread"));
    assert(latestAfterDanglingPointer.isOk());
    assert(latestAfterDanglingPointer->has_value());
    assert((*latestAfterDanglingPointer)->checkpoint_.checkpointId_ == "cp-2");

    assert(storage->put(
        latestPointerKey("crash-thread"),
        "cp-1",
        lgc::StoragePutOptions { .mode_ = lgc::StoragePutMode::Upsert })
            .isOk());
    auto latestAfterStalePointer = checkpointer.getTuple(lgc::CheckpointQuery::latest("crash-thread"));
    assert(latestAfterStalePointer.isOk());
    assert(latestAfterStalePointer->has_value());
    assert((*latestAfterStalePointer)->checkpoint_.checkpointId_ == "cp-2");
}

void testPartialPendingWritesSurviveReopen()
{
    const auto path = uniqueDatabasePath("partial-writes");
    removeDatabaseFiles(path);
    {
        auto storage = openSQLite(path.string());
        lgc::StorageSaver checkpointer(storage);
        assert(checkpointer.put(checkpoint("cp-1", 1, R"({"value":1})")).isOk());

        lgc::CheckpointWrite partialWrite {
            .taskId_ = "task-1",
            .taskPath_ = "branch",
            .nodeId_ = "left",
            .update_ = stateFromJson(R"({"left":true})"),
            .order_ = 0,
        };
        assert(storage->put(
            checkpointWriteKey("crash-thread", {}, "cp-1", "task-1", "branch", 0),
            storageValueForWrite(partialWrite),
            lgc::StoragePutOptions { .mode_ = lgc::StoragePutMode::Upsert })
                .isOk());
        assert(storage->flush().isOk());
    }

    auto reopened = openSQLite(path.string());
    lgc::StorageSaver checkpointer(reopened);
    auto latest = checkpointer.getTuple(lgc::CheckpointQuery::latest("crash-thread"));
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->pendingWrites_.size() == 1);
    assert((*latest)->pendingWrites_.front().nodeId_ == "left");
    assert((*latest)->pendingWrites_.front().update_.view().at("left") == true);
    assert((*latest)->checkpoint_.pendingWrites_.size() == 1);
}

void testMultiProcessContention(const char* executable)
{
    const auto path = uniqueDatabasePath("contention");
    removeDatabaseFiles(path);

    std::vector<std::future<void>> writers;
    for (int i = 0; i < 4; ++i) {
        writers.push_back(std::async(std::launch::async, [executable, path, i] {
            runSuccessfulChild(executable, path, "writer:" + std::to_string(i));
        }));
    }
    for (auto& writer : writers)
        writer.get();

    auto storage = openSQLite(path.string());
    lgc::StorageSaver checkpointer(storage);
    lgc::StorageStore store(storage);
    for (int i = 0; i < 4; ++i) {
        const std::string threadId = "contention-thread-" + std::to_string(i);
        auto latest = checkpointer.getTuple(lgc::CheckpointQuery::latest(threadId));
        assert(latest.isOk());
        assert(latest->has_value());
        assert((*latest)->checkpoint_.checkpointId_ == "cp-8");
        assert((*latest)->checkpoint_.state_.view().at("value") == 8);

        auto item = store.get({ "edge", threadId }, "memory");
        assert(item.isOk());
        assert(item->has_value());
        assert((*item)->value_.at("durable") == true);
    }
}

void testPowerLossStyleKillDuringWrites(const char* executable)
{
    const auto path = uniqueDatabasePath("kill-loop");
    removeDatabaseFiles(path);
    runKilledChild(executable, path, "kill-loop:wal:full");

    auto storage = openSQLite(path.string());
    lgc::StorageSaver checkpointer(storage);
    auto latest = checkpointer.getTuple(lgc::CheckpointQuery::latest("kill-thread"));
    assert(latest.isOk());
    assert(latest->has_value());
    assert((*latest)->checkpoint_.state_.view().contains("value"));

    auto history = checkpointer.list(lgc::CheckpointListOptions {
        .threadId_ = "kill-thread",
        .checkpointNamespace_ = std::string(),
        .order_ = lgc::CheckpointListOrder::OldestFirst,
    });
    assert(history.isOk());
    assert(!history->empty());
    for (const auto& record : *history)
        assert(record.checkpoint_.state_.view().contains("value"));

    lgc::StorageStore store(storage);
    auto item = store.get({ "edge" }, "kill-marker");
    assert(item.isOk());
    assert(item->has_value());
    assert((*item)->value_.at("started") == true);
}

void testCorruptionIsReported()
{
    const auto path = uniqueDatabasePath("corrupt");
    removeDatabaseFiles(path);
    auto storage = openSQLite(path.string());

    lgc::StorageSaver checkpointer(storage);
    assert(checkpointer.put(checkpoint("cp-1", 1, R"({"value":1})")).isOk());
    assert(storage->put(
        checkpointKey("crash-thread", "cp-1"),
        "not-json",
        lgc::StoragePutOptions { .mode_ = lgc::StoragePutMode::Upsert })
        .isOk());
    auto corruptedCheckpoint = checkpointer.getTuple(lgc::CheckpointQuery::at("crash-thread", "cp-1"));
    assert(!corruptedCheckpoint.isOk());
    assert(corruptedCheckpoint.status().code() == lgc::StatusCode::InvalidArgument);
    auto corruptedCheckpointList = checkpointer.list(lgc::CheckpointListOptions {
        .threadId_ = "crash-thread",
        .checkpointNamespace_ = std::string(),
    });
    assert(!corruptedCheckpointList.isOk());
    assert(corruptedCheckpointList.status().code() == lgc::StatusCode::InvalidArgument);

    lgc::StorageStore store(storage);
    assert(store.put({ "edge" }, "memory", { { "ok", true } }).isOk());
    assert(storage->put(
        storeKey("memory"),
        "[",
        lgc::StoragePutOptions { .mode_ = lgc::StoragePutMode::Upsert })
        .isOk());
    auto corruptedStore = store.get({ "edge" }, "memory");
    assert(!corruptedStore.isOk());
    assert(corruptedStore.status().code() == lgc::StatusCode::InvalidArgument);
    auto corruptedStoreSearch = store.search(lgc::StoreSearchOptions {
        .namespacePrefix_ = { "edge" },
    });
    assert(!corruptedStoreSearch.isOk());
    assert(corruptedStoreSearch.status().code() == lgc::StatusCode::InvalidArgument);
}

void testRetentionAndPruning()
{
    const auto path = uniqueDatabasePath("prune");
    removeDatabaseFiles(path);
    auto storage = openSQLite(path.string());
    lgc::StorageSaver checkpointer(storage);
    assert(checkpointer.put(checkpoint("cp-1", 1, R"({"value":1})")).isOk());
    assert(checkpointer.put(checkpoint("cp-2", 2, R"({"value":2})")).isOk());
    assert(checkpointer.put(checkpoint("cp-3", 3, R"({"value":3})")).isOk());

    auto pruned = checkpointer.prune(
        "crash-thread",
        lgc::CheckpointPruneOptions {
            .keepLatest_ = 1,
        });
    assert(pruned.isOk());
    assert(pruned->removed_ == 2);
    assert(pruned->remaining_ == 1);
    assert(pruned->latestCheckpointId_ == "cp-3");

    auto history = checkpointer.list(lgc::CheckpointListOptions {
        .threadId_ = "crash-thread",
        .checkpointNamespace_ = std::string(),
        .order_ = lgc::CheckpointListOrder::OldestFirst,
    });
    assert(history.isOk());
    assert(history->size() == 1);
    assert(history->front().checkpoint_.checkpointId_ == "cp-3");
}

#endif

} // namespace

int main(int argc, char** argv)
{
#if LANGGRAPH_CPP_WITH_SQLITE
    if (argc == 4 && std::string_view(argv[1]) == "--crash-child")
        return crashChild(argv[2], argv[3]);

    assert(argc >= 1);
    testReopenAfterHardExit(argv[0]);
    testJournalAndSynchronousHardExitMatrix(argv[0]);
    testPartialCheckpointWriteReconcilesLatestPointer(argv[0]);
    testDanglingAndStaleLatestPointerReconcileByScan();
    testPartialPendingWritesSurviveReopen();
    testMultiProcessContention(argv[0]);
    testPowerLossStyleKillDuringWrites(argv[0]);
    testCorruptionIsReported();
    testRetentionAndPruning();
#else
    (void)argc;
    (void)argv;
#endif
    return 0;
}
