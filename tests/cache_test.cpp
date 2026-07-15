#include "foundation/cache/cache.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] std::string hexEncode(std::string_view value)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto ch : value)
        out << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(ch));
    return out.str();
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(file);
    file << text;
    assert(file);
}

[[nodiscard]] std::filesystem::path diskEntryPath(
    const std::filesystem::path& root,
    std::string_view namespaceName,
    std::string_view key)
{
    return root / hexEncode(namespaceName) / (hexEncode(key) + ".json");
}

} // namespace

int main()
{
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

    lgc::ManualClock clock;

    {
        lgc::MemoryCache cache(lgc::CacheOptions {
            .maxEntries_ = 2,
            .defaultTtl_ = 1s,
        }, clock);

        assert(cache.put({ "prompt", "system" }, "be helpful").isOk());
        auto value = cache.get({ "prompt", "system" });
        assert(value.isOk());
        assert(*value == "be helpful");

        clock.advance(999ms);
        assert(cache.get({ "prompt", "system" }).isOk());
        clock.advance(1ms);
        assert(cache.get({ "prompt", "system" }).status().code() == lgc::StatusCode::NotFound);
        assert(cache.size() == 0);

        assert(cache.put({ "model", "a" }, "A", lgc::CacheWriteOptions { .ttl_ = 5s }).isOk());
        assert(cache.put({ "model", "b" }, "B", lgc::CacheWriteOptions { .ttl_ = 5s }).isOk());
        assert(cache.get({ "model", "a" }).isOk());
        assert(cache.put({ "model", "c" }, "C", lgc::CacheWriteOptions { .ttl_ = 5s }).isOk());
        assert(cache.get({ "model", "a" }).isOk());
        assert(cache.get({ "model", "b" }).status().code() == lgc::StatusCode::NotFound);
        assert(cache.get({ "model", "c" }).isOk());

        assert(cache.clearNamespace("model").isOk());
        assert(cache.size() == 0);
    }

    {
        lgc::MemoryCache cache;
        assert(cache.put({ "a", "b\nc" }, "left").isOk());
        assert(cache.put({ "a\nb", "c" }, "right").isOk());
        auto left = cache.get({ "a", "b\nc" });
        auto right = cache.get({ "a\nb", "c" });
        assert(left.isOk());
        assert(right.isOk());
        assert(*left == "left");
        assert(*right == "right");

        assert(cache.put({ "", "" }, "x").code() == lgc::StatusCode::InvalidArgument);
        assert(cache.put({ "ns", "key" }, "x", lgc::CacheWriteOptions { .ttl_ = -1s }).code()
            == lgc::StatusCode::InvalidArgument);
    }

    {
        lgc::MemoryCache cache(lgc::CacheOptions {
            .maxEntries_ = 10,
            .maxValueBytes_ = 4,
        });
        assert(cache.put({ "limit", "small" }, "1234").isOk());
        assert(cache.put({ "limit", "large" }, "12345").code() == lgc::StatusCode::ResourceExhausted);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_test";
        fs::remove_all(root);

        lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 10 });
        assert(cache.put({ "http", "GET /v1/models" }, R"({"ok":true})").isOk());

        auto value = cache.get({ "http", "GET /v1/models" });
        assert(value.isOk());
        assert(*value == R"({"ok":true})");

        lgc::DiskCache reopened(root, lgc::CacheOptions { .maxEntries_ = 10 });
        value = reopened.get({ "http", "GET /v1/models" });
        assert(value.isOk());
        assert(*value == R"({"ok":true})");

        assert(reopened.put({ "tool", "schema-a" }, "A").isOk());
        assert(reopened.put({ "tool", "schema-b" }, "B").isOk());
        assert(reopened.size() == 3);

        assert(reopened.clearNamespace("tool").isOk());
        assert(reopened.get({ "tool", "schema-b" }).status().code() == lgc::StatusCode::NotFound);
        assert(reopened.get({ "http", "GET /v1/models" }).isOk());

        assert(reopened.remove({ "http", "GET /v1/models" }).isOk());
        assert(reopened.remove({ "http", "GET /v1/models" }).code() == lgc::StatusCode::NotFound);

        const auto corruptDir = root / "636f7272757074";
        fs::create_directories(corruptDir);
        {
            std::ofstream file(corruptDir / "656e747279.json");
            file << "{not-json";
        }
        assert(reopened.get({ "corrupt", "entry" }).status().code() == lgc::StatusCode::DataLoss);

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_ttl_test";
        fs::remove_all(root);

        lgc::ManualClock diskClock;
        lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 10 }, diskClock);
        assert(cache.put({ "ttl", "entry" }, "value", lgc::CacheWriteOptions { .ttl_ = 10ms }).isOk());
        assert(cache.get({ "ttl", "entry" }).isOk());
        diskClock.advance(9ms);
        assert(cache.get({ "ttl", "entry" }).isOk());
        diskClock.advance(1ms);
        assert(cache.get({ "ttl", "entry" }).status().code() == lgc::StatusCode::NotFound);
        assert(cache.size() == 0);

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_schema_test";
        fs::remove_all(root);

        lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 10 });
        assert(cache.put({ "seed", "entry" }, "value").isOk());
        assert(cache.remove({ "seed", "entry" }).isOk());

        writeText(
            diskEntryPath(root, "bad", "expires"),
            R"({"namespace":"bad","key":"expires","value":"x","expires_at_unix_ms":"soon"})");
        assert(cache.get({ "bad", "expires" }).status().code() == lgc::StatusCode::DataLoss);

        writeText(
            diskEntryPath(root, "bad", "missing-value"),
            R"({"namespace":"bad","key":"missing-value","expires_at_unix_ms":null})");
        assert(cache.get({ "bad", "missing-value" }).status().code() == lgc::StatusCode::DataLoss);

        writeText(
            diskEntryPath(root, "bad", "mismatch"),
            R"({"namespace":"other","key":"mismatch","value":"x","expires_at_unix_ms":null})");
        assert(cache.get({ "bad", "mismatch" }).status().code() == lgc::StatusCode::DataLoss);

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_limits_test";
        fs::remove_all(root);

        lgc::DiskCache cache(root, lgc::CacheOptions {
            .maxEntries_ = 10,
            .maxValueBytes_ = 4,
        });
        assert(cache.put({ "limit", "small" }, "1234").isOk());
        assert(cache.put({ "limit", "large" }, "12345").code() == lgc::StatusCode::ResourceExhausted);

        lgc::DiskCache tinyDisk(root / "tiny", lgc::CacheOptions {
            .maxEntries_ = 10,
            .maxDiskSizeBytes_ = 1,
        });
        assert(tinyDisk.put({ "disk", "entry" }, "value").isOk());
        assert(tinyDisk.get({ "disk", "entry" }).status().code() == lgc::StatusCode::NotFound);
        assert(tinyDisk.size() == 0);

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_lru_test";
        fs::remove_all(root);

        lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 2 });
        assert(cache.put({ "lru", "a" }, "A").isOk());
        std::this_thread::sleep_for(20ms);
        assert(cache.put({ "lru", "b" }, "B").isOk());
        std::this_thread::sleep_for(20ms);
        assert(cache.get({ "lru", "a" }).isOk());
        std::this_thread::sleep_for(20ms);
        assert(cache.put({ "lru", "c" }, "C").isOk());

        assert(cache.get({ "lru", "a" }).isOk());
        assert(cache.get({ "lru", "b" }).status().code() == lgc::StatusCode::NotFound);
        assert(cache.get({ "lru", "c" }).isOk());

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_marker_test";
        fs::remove_all(root);
        fs::create_directories(root);
        writeText(root / "important.txt", "do not delete");

        lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 10 });
        assert(cache.clear().code() == lgc::StatusCode::FailedPrecondition);
        assert(fs::exists(root / "important.txt"));

        assert(cache.put({ "safe", "entry" }, "value").isOk());
        assert(fs::exists(root / ".langgraph-cache"));
        assert(cache.clear().isOk());
        assert(!fs::exists(root));
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_scan_test";
        fs::remove_all(root);

        lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 1 });
        assert(cache.put({ "own", "a" }, "A").isOk());

        writeText(root / "note.json", "not a cache entry");
        writeText(root / ".tmp-entry.json.tmp", "temporary");
        writeText(root / "nothex" / "aaaa.json", "wrong namespace directory");
        writeText(root / "abcd" / "entry.tmp", "wrong extension");

        assert(cache.size() == 1);
        assert(cache.put({ "own", "b" }, "B").isOk());
        assert(cache.size() == 1);
        writeText(root / "abcd" / ".tmp-656e747279.json-stale.tmp", "temporary");
        assert(cache.compact().isOk());
        assert(!fs::exists(root / "abcd" / ".tmp-656e747279.json-stale.tmp"));
        assert(fs::exists(root / "note.json"));
        assert(fs::exists(root / ".tmp-entry.json.tmp"));
        assert(fs::exists(root / "nothex" / "aaaa.json"));
        assert(fs::exists(root / "abcd" / "entry.tmp"));

        fs::remove_all(root);
    }

    {
        lgc::MemoryCache cache(lgc::CacheOptions { .maxEntries_ = 1024 });
        std::atomic<int> failures { 0 };
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&cache, &failures, worker] {
                for (int i = 0; i < 100; ++i) {
                    const lgc::CacheKey key {
                        .namespace_ = "worker-" + std::to_string(worker),
                        .key_ = "key-" + std::to_string(i),
                    };
                    const auto value = "value-" + std::to_string(worker) + "-" + std::to_string(i);
                    if (!cache.put(key, value).isOk()) {
                        ++failures;
                        continue;
                    }
                    auto read = cache.get(key);
                    if (!read.isOk() || *read != value)
                        ++failures;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(failures.load() == 0);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_concurrency_test";
        fs::remove_all(root);

        lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 1024 });
        std::atomic<int> failures { 0 };
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&cache, &failures, worker] {
                for (int i = 0; i < 40; ++i) {
                    const lgc::CacheKey key {
                        .namespace_ = "worker-" + std::to_string(worker),
                        .key_ = "key-" + std::to_string(i),
                    };
                    const auto value = "value-" + std::to_string(worker) + "-" + std::to_string(i);
                    if (!cache.put(key, value).isOk()) {
                        ++failures;
                        continue;
                    }
                    auto read = cache.get(key);
                    if (!read.isOk() || *read != value)
                        ++failures;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(failures.load() == 0);

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_cache_multi_instance_test";
        fs::remove_all(root);

        std::atomic<int> failures { 0 };
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([root, &failures, worker] {
                lgc::DiskCache cache(root, lgc::CacheOptions { .maxEntries_ = 1024 });
                for (int i = 0; i < 40; ++i) {
                    const lgc::CacheKey key {
                        .namespace_ = "instance-" + std::to_string(worker),
                        .key_ = "key-" + std::to_string(i),
                    };
                    const auto value = "value-" + std::to_string(worker) + "-" + std::to_string(i);
                    if (!cache.put(key, value).isOk()) {
                        ++failures;
                        continue;
                    }
                    auto read = cache.get(key);
                    if (!read.isOk() || *read != value)
                        ++failures;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(failures.load() == 0);

        lgc::DiskCache verify(root, lgc::CacheOptions { .maxEntries_ = 1024 });
        assert(verify.size() == 160);

        fs::remove_all(root);
        fs::remove(root.string() + ".langgraph-cache.lock");
    }

    return 0;
}
