#include "foundation/blob/blob_store.hpp"
#include "foundation/crypto/crypto.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

lc::BlobData bytes(std::initializer_list<unsigned int> values)
{
    lc::BlobData out;
    out.reserve(values.size());
    for (const auto value : values)
        out.push_back(std::byte(value));
    return out;
}

lc::BlobData bytesFromText(std::string_view text)
{
    lc::BlobData out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

std::string textFromBytes(const lc::BlobData& data)
{
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(file);
    file << text;
    assert(file);
}

std::string sha256Text(std::string_view text)
{
    auto checksum = lc::digestHex(lc::HashAlgorithm::Sha256, text);
    assert(checksum.isOk());
    return *checksum;
}

std::filesystem::path metadataPath(
    const std::filesystem::path& root,
    const lc::BlobKey& key)
{
    return root / key.namespace_ / (key.name_ + ".meta.json");
}

std::filesystem::path contentPath(
    const std::filesystem::path& root,
    const lc::BlobKey& key,
    std::string_view checksum)
{
    return root / key.namespace_ / (key.name_ + "." + std::string(checksum) + ".data");
}

} // namespace

int main()
{
    namespace fs = std::filesystem;

    {
        lc::MemoryBlobStore store;
        const lc::BlobKey key {
            .namespace_ = "run-1",
            .name_ = "images/chart.png",
        };

        auto data = bytes({ 0x89, 0x50, 0x4e, 0x47 });
        assert(store.put(
            key,
            data,
            lc::BlobPutOptions {
                .contentType_ = "image/png",
                .metadata_ = { { "kind", "chart" } },
            })
                .isOk());

        auto loaded = store.get(key);
        assert(loaded.isOk());
        assert(loaded->has_value());
        assert((*loaded)->data_ == data);
        assert((*loaded)->info_.contentType_ == "image/png");
        assert((*loaded)->info_.metadata_.at("kind") == "chart");
        assert((*loaded)->info_.size_ == data.size());
        assert((*loaded)->info_.checksumSha256_ == sha256Text(std::string_view("\x89PNG", 4)));
        auto infoOnly = store.stat(key);
        assert(infoOnly.isOk());
        assert(infoOnly->has_value());
        assert((*infoOnly)->key_ == key);
        assert((*infoOnly)->size_ == data.size());

        std::string streamed;
        assert(store.read(key, [&](std::span<const std::byte> chunk) {
            streamed.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            return lc::Status::ok();
        }, lc::BlobReadOptions { .chunkBytes_ = 2 }).isOk());
        assert(streamed == std::string_view("\x89PNG", 4));
        assert(store.read(key, {}, lc::BlobReadOptions {}).status().code() == lc::StatusCode::InvalidArgument);
        assert(lc::validateBlobReadOptions(lc::BlobReadOptions { .chunkBytes_ = 0 }).code()
            == lc::StatusCode::InvalidArgument);

        assert(store.put(key, data, lc::BlobPutOptions { .replace_ = false }).status().code()
            == lc::StatusCode::AlreadyExists);
        assert(store.put(
                    { "run-1", "bad-content-type" },
                    data,
                    lc::BlobPutOptions { .contentType_ = "not-a-media-type" })
                   .status()
                   .code()
            == lc::StatusCode::InvalidArgument);

        assert(store.put({ "run-1", "logs/stdout.txt" }, bytesFromText("stdout")).isOk());
        assert(store.put({ "run-1", "tool/output.json" }, bytesFromText("{}")).isOk());
        assert(store.put({ "run-2", "logs/stdout.txt" }, bytesFromText("other")).isOk());

        auto page = store.list(lc::BlobListOptions {
            .namespace_ = "run-1",
            .limit_ = 2,
        });
        assert(page.isOk());
        assert(page->items_.size() == 2);
        assert(!page->nextCursor_.empty());

        auto next = store.list(lc::BlobListOptions {
            .namespace_ = "run-1",
            .limit_ = 2,
            .cursor_ = page->nextCursor_,
        });
        assert(next.isOk());
        assert(next->items_.size() == 1);

        auto prefixed = store.list(lc::BlobListOptions {
            .namespace_ = "run-1",
            .namePrefix_ = "logs/",
        });
        assert(prefixed.isOk());
        assert(prefixed->items_.size() == 1);
        assert(prefixed->items_[0].key_.name_ == "logs/stdout.txt");

        assert(store.remove(key).isOk());
        assert(!store.get(key)->has_value());
        assert(store.clearNamespace("run-1").isOk());
        assert(store.list(lc::BlobListOptions { .namespace_ = "run-1" })->items_.empty());
        assert(store.list(lc::BlobListOptions { .namespace_ = "run-2" })->items_.size() == 1);
    }

    {
        const auto source = fs::temp_directory_path() / "langgraph_cpp_blob_memory_source.txt";
        writeText(source, "memory file");
        lc::MemoryBlobStore store;
        const lc::BlobKey key { "run-file", "source.txt" };
        assert(store.putFile(key, source, lc::BlobPutOptions { .contentType_ = "text/plain" }).isOk());
        auto loaded = store.get(key);
        assert(loaded.isOk());
        assert(loaded->has_value());
        assert(textFromBytes((*loaded)->data_) == "memory file");
        fs::remove(source);
    }

    {
        assert(lc::validateBlobKey({ "", "x" }).code() == lc::StatusCode::InvalidArgument);
        assert(lc::validateBlobKey({ "run", "../x" }).code() == lc::StatusCode::PermissionDenied);
        assert(lc::validateBlobListOptions(lc::BlobListOptions { .limit_ = 0 }).code()
            == lc::StatusCode::InvalidArgument);
        assert(lc::validateBlobListOptions(lc::BlobListOptions { .namePrefix_ = "../x" }).code()
            == lc::StatusCode::PermissionDenied);
        assert(lc::decodeBlobCursor("bad").status().code() == lc::StatusCode::InvalidArgument);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_blob_store_test";
        fs::remove_all(root);

        lc::FileSystemBlobStore store(root);
        const lc::BlobKey key {
            .namespace_ = "run-1",
            .name_ = "artifacts/output.txt",
        };
        assert(store.put(
            key,
            bytesFromText("hello artifact"),
            lc::BlobPutOptions {
                .contentType_ = "text/plain",
                .metadata_ = { { "source", "tool" } },
            })
                .isOk());

        lc::FileSystemBlobStore reopened(root);
        auto loaded = reopened.get(key);
        assert(loaded.isOk());
        assert(loaded->has_value());
        assert(textFromBytes((*loaded)->data_) == "hello artifact");
        assert((*loaded)->info_.contentType_ == "text/plain");
        assert((*loaded)->info_.metadata_.at("source") == "tool");
        const auto originalChecksum = (*loaded)->info_.checksumSha256_;
        auto infoOnly = reopened.stat(key);
        assert(infoOnly.isOk());
        assert(infoOnly->has_value());
        assert((*infoOnly)->checksumSha256_ == originalChecksum);

        std::string streamed;
        assert(reopened.read(key, [&](std::span<const std::byte> chunk) {
            streamed.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            return lc::Status::ok();
        }, lc::BlobReadOptions { .chunkBytes_ = 5 }).isOk());
        assert(streamed == "hello artifact");

        const auto fileSource = root / "source-file.txt";
        writeText(fileSource, "file backed artifact");
        const lc::BlobKey fileKey { "run-1", "artifacts/from-file.txt" };
        assert(reopened.putFile(
            fileKey,
            fileSource,
            lc::BlobPutOptions {
                .contentType_ = "text/plain",
                .metadata_ = { { "source", "file" } },
            }).isOk());
        streamed.clear();
        assert(reopened.read(fileKey, [&](std::span<const std::byte> chunk) {
            streamed.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            return lc::Status::ok();
        }, lc::BlobReadOptions { .chunkBytes_ = 3 }).isOk());
        assert(streamed == "file backed artifact");
        auto fileInfo = reopened.stat(fileKey);
        assert(fileInfo.isOk());
        assert(fileInfo->has_value());
        assert((*fileInfo)->metadata_.at("source") == "file");

        auto listed = reopened.list(lc::BlobListOptions { .namespace_ = "run-1" });
        assert(listed.isOk());
        assert(listed->items_.size() == 2);

        writeText(root / "run-1" / "notes.json", "{not-json");
        writeText(root / "run-1" / "notes.meta.json.tmp", "{not-json");
        listed = reopened.list(lc::BlobListOptions { .namespace_ = "run-1" });
        assert(listed.isOk());
        assert(listed->items_.size() == 2);
        fs::remove(root / "run-1" / "notes.json");
        fs::remove(root / "run-1" / "notes.meta.json.tmp");

        assert(reopened.get({ "run-1", "../outside.txt" }).status().code() == lc::StatusCode::PermissionDenied);

        const auto metadata = metadataPath(root, key);
        {
            std::ofstream file(metadata, std::ios::trunc);
            file << "{not-json";
        }
        assert(reopened.get(key).status().code() == lc::StatusCode::DataLoss);

        auto overwriteCorrupt = reopened.put(
            key,
            bytesFromText("restored artifact"),
            lc::BlobPutOptions { .contentType_ = "text/plain" });
        assert(!overwriteCorrupt.isOk());
        assert(overwriteCorrupt.status().code() == lc::StatusCode::DataLoss);

        fs::remove(contentPath(root, key, originalChecksum));
        fs::remove(metadata);
        assert(reopened.put(
            key,
            bytesFromText("restored artifact"),
            lc::BlobPutOptions { .contentType_ = "text/plain" })
                .isOk());
        auto restored = reopened.get(key);
        assert(restored.isOk());
        assert(restored->has_value());
        const auto restoredChecksum = (*restored)->info_.checksumSha256_;
        fs::remove(contentPath(root, key, restoredChecksum));
        assert(reopened.get(key).status().code() == lc::StatusCode::DataLoss);
        fs::remove(metadata);

        writeText(root / "run-1" / "bad-schema.txt.data", "bad");
        writeText(
            root / "run-1" / "bad-schema.txt.meta.json",
            R"({"namespace":"run-1","name":"bad-schema.txt","size":3,"metadata":{},"created_at_unix_ms":0,"updated_at_unix_ms":0})");
        assert(reopened.get({ "run-1", "bad-schema.txt" }).status().code() == lc::StatusCode::DataLoss);
        fs::remove(root / "run-1" / "bad-schema.txt.data");
        fs::remove(root / "run-1" / "bad-schema.txt.meta.json");

        const auto emptyChecksum = sha256Text("");
        writeText(
            root / "run-1" / "bad-content-type.txt.meta.json",
            "{\"namespace\":\"run-1\",\"name\":\"bad-content-type.txt\",\"content_type\":\"not-a-media-type\",\"size\":0,"
            "\"checksum_sha256\":\""
                + emptyChecksum
                + "\",\"metadata\":{},\"created_at_unix_ms\":0,\"updated_at_unix_ms\":0}");
        writeText(contentPath(root, { "run-1", "bad-content-type.txt" }, emptyChecksum), "");
        assert(reopened.get({ "run-1", "bad-content-type.txt" }).status().code() == lc::StatusCode::DataLoss);
        fs::remove(root / "run-1" / "bad-content-type.txt.meta.json");
        fs::remove(contentPath(root, { "run-1", "bad-content-type.txt" }, emptyChecksum));

        const lc::BlobKey badSizeKey { "run-1", "bad-size.txt" };
        const auto badSizeChecksum = sha256Text("abc");
        writeText(contentPath(root, badSizeKey, badSizeChecksum), "abc");
        writeText(
            metadataPath(root, badSizeKey),
            "{\"namespace\":\"run-1\",\"name\":\"bad-size.txt\",\"content_type\":\"text/plain\",\"size\":4,"
            "\"checksum_sha256\":\""
                + badSizeChecksum
                + "\",\"metadata\":{},\"created_at_unix_ms\":0,\"updated_at_unix_ms\":0}");
        assert(reopened.get(badSizeKey).status().code() == lc::StatusCode::DataLoss);
        assert(reopened.list(lc::BlobListOptions { .namespace_ = "run-1" }).status().code()
            == lc::StatusCode::DataLoss);
        fs::remove(contentPath(root, badSizeKey, badSizeChecksum));
        fs::remove(metadataPath(root, badSizeKey));

        writeText(
            root / "run-1" / "wrong-path.meta.json",
            "{\"namespace\":\"run-1\",\"name\":\"other.txt\",\"content_type\":\"text/plain\",\"size\":0,"
            "\"checksum_sha256\":\""
                + emptyChecksum
                + "\",\"metadata\":{},\"created_at_unix_ms\":0,\"updated_at_unix_ms\":0}");
        writeText(contentPath(root, { "run-1", "other.txt" }, emptyChecksum), "");
        assert(reopened.list(lc::BlobListOptions { .namespace_ = "run-1" }).status().code()
            == lc::StatusCode::DataLoss);
        fs::remove(root / "run-1" / "wrong-path.meta.json");
        fs::remove(contentPath(root, { "run-1", "other.txt" }, emptyChecksum));

        const auto orphanChecksum = sha256Text("orphan");
        const auto orphanPath = contentPath(root, { "run-1", "orphan" }, orphanChecksum);
        const auto tempPath = root / "run-1" / ".blob-stale.tmp";
        writeText(orphanPath, "orphan");
        writeText(tempPath, "temporary");
        auto orphanData = reopened.get({ "run-1", "orphan" });
        assert(orphanData.isOk());
        assert(!orphanData->has_value());
        assert(reopened.compact().isOk());
        assert(!fs::exists(orphanPath));
        assert(!fs::exists(tempPath));

        const lc::BlobKey missingDataKey { "run-1", "missing-data.txt" };
        const auto missingDataChecksum = sha256Text("missing");
        writeText(
            metadataPath(root, missingDataKey),
            "{\"namespace\":\"run-1\",\"name\":\"missing-data.txt\",\"content_type\":\"text/plain\",\"size\":7,"
            "\"checksum_sha256\":\""
                + missingDataChecksum
                + "\",\"metadata\":{},\"created_at_unix_ms\":0,\"updated_at_unix_ms\":0}");
        assert(reopened.get(missingDataKey).status().code() == lc::StatusCode::DataLoss);
        assert(reopened.list(lc::BlobListOptions { .namespace_ = "run-1" }).status().code()
            == lc::StatusCode::DataLoss);
        fs::remove(metadataPath(root, missingDataKey));

        assert(reopened.put({ "run-1", "tamper.txt" }, bytesFromText("abcd")).isOk());
        auto tampered = reopened.get({ "run-1", "tamper.txt" });
        assert(tampered.isOk());
        assert(tampered->has_value());
        writeText(contentPath(root, { "run-1", "tamper.txt" }, (*tampered)->info_.checksumSha256_), "wxyz");
        assert(reopened.get({ "run-1", "tamper.txt" }).status().code() == lc::StatusCode::DataLoss);
        fs::remove(metadataPath(root, { "run-1", "tamper.txt" }));
        fs::remove(contentPath(root, { "run-1", "tamper.txt" }, (*tampered)->info_.checksumSha256_));

        const auto foreignChecksum = sha256Text("broken");
        writeText(
            metadataPath(root, { "run-2", "broken.txt" }),
            "{\"namespace\":\"run-2\",\"name\":\"broken.txt\",\"content_type\":\"text/plain\",\"size\":99,"
            "\"checksum_sha256\":\""
                + foreignChecksum
                + "\",\"metadata\":{},\"created_at_unix_ms\":0,\"updated_at_unix_ms\":0}");
        auto scopedList = reopened.list(lc::BlobListOptions { .namespace_ = "run-1" });
        assert(scopedList.isOk());
        assert(reopened.list().status().code() == lc::StatusCode::DataLoss);
        fs::remove_all(root / "run-2");

        assert(reopened.put(
            key,
            bytesFromText("hello artifact"),
            lc::BlobPutOptions { .contentType_ = "text/plain" })
                .isOk());
        assert(reopened.remove(key).isOk());
        assert(!reopened.get(key)->has_value());

        assert(reopened.put({ "run-1", "a.txt" }, bytesFromText("a")).isOk());
        assert(reopened.put({ "run-1", "b.txt" }, bytesFromText("b")).isOk());
        for (int i = 0; i < 64; ++i) {
            assert(reopened.put(
                { "run-page", "checkpoint-" + std::to_string(1000 + i) + ".bin" },
                bytesFromText("x"))
                       .isOk());
        }
        std::string cursor;
        int seen = 0;
        do {
            auto page = reopened.list(lc::BlobListOptions {
                .namespace_ = "run-page",
                .namePrefix_ = "checkpoint-",
                .limit_ = 7,
                .cursor_ = cursor,
            });
            assert(page.isOk());
            seen += static_cast<int>(page->items_.size());
            cursor = page->nextCursor_;
        } while (!cursor.empty());
        assert(seen == 64);

        assert(reopened.clearNamespace("run-1").isOk());
        assert(reopened.list(lc::BlobListOptions { .namespace_ = "run-1" })->items_.empty());
        assert(reopened.clearNamespace("run-page").isOk());

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_blob_store_marker_test";
        fs::remove_all(root);
        writeText(root / "run-1" / "important.txt", "do not delete");

        lc::FileSystemBlobStore store(root);
        assert(store.clearNamespace("run-1").status().code() == lc::StatusCode::FailedPrecondition);
        assert(fs::exists(root / "run-1" / "important.txt"));

        assert(store.put({ "run-1", "artifact.txt" }, bytesFromText("value")).isOk());
        assert(fs::exists(root / ".langgraph-blob-store"));
        assert(store.clearNamespace("run-1").isOk());
        assert(!fs::exists(root / "run-1"));

        fs::remove_all(root);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_blob_store_bad_marker_test";
        fs::remove_all(root);
        writeText(root / ".langgraph-blob-store", "not this store\n");

        lc::FileSystemBlobStore store(root);
        assert(store.put({ "run-1", "artifact.txt" }, bytesFromText("value")).status().code()
            == lc::StatusCode::FailedPrecondition);
        assert(store.clearNamespace("run-1").status().code() == lc::StatusCode::FailedPrecondition);

        fs::remove_all(root);
    }

    {
        lc::MemoryBlobStore store(lc::BlobStoreOptions { .maxBlobBytes_ = 4 });
        assert(store.put({ "limit", "too-large" }, bytesFromText("12345")).status().code()
            == lc::StatusCode::ResourceExhausted);
        assert(store.put({ "limit", "ok" }, bytesFromText("1234")).isOk());
    }

    {
        lc::MemoryBlobStore store(lc::BlobStoreOptions { .maxListItems_ = 1 });
        assert(store.put({ "limit", "a" }, bytesFromText("a")).isOk());
        assert(store.list(lc::BlobListOptions { .namespace_ = "limit", .limit_ = 2 }).status().code()
            == lc::StatusCode::ResourceExhausted);
    }

    {
        lc::MemoryBlobStore store(lc::BlobStoreOptions { .maxMetadataEntries_ = 1 });
        assert(store.put(
                    { "limit", "too-many-metadata" },
                    bytesFromText("x"),
                    lc::BlobPutOptions {
                        .metadata_ = { { "a", "1" }, { "b", "2" } },
                    })
                   .status()
                   .code()
            == lc::StatusCode::ResourceExhausted);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_blob_store_limit_test";
        fs::remove_all(root);

        lc::FileSystemBlobStore store(root, lc::BlobStoreOptions { .maxBlobBytes_ = 4 });
        assert(store.put({ "limit", "too-large" }, bytesFromText("12345")).status().code()
            == lc::StatusCode::ResourceExhausted);
        assert(store.put({ "limit", "ok" }, bytesFromText("1234")).isOk());
        assert(store.get({ "limit", "ok" }).isOk());

        fs::remove_all(root);
    }

    {
        lc::MemoryBlobStore store;
        std::atomic<int> failures { 0 };
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&store, &failures, worker] {
                for (int i = 0; i < 100; ++i) {
                    const lc::BlobKey key {
                        .namespace_ = "worker-" + std::to_string(worker),
                        .name_ = "blob-" + std::to_string(i),
                    };
                    const auto value = "value-" + std::to_string(worker) + "-" + std::to_string(i);
                    if (!store.put(key, bytesFromText(value)).isOk()) {
                        ++failures;
                        continue;
                    }
                    auto loaded = store.get(key);
                    if (!loaded.isOk() || !loaded->has_value() || textFromBytes((*loaded)->data_) != value)
                        ++failures;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(failures.load() == 0);
    }

    {
        const auto root = fs::temp_directory_path() / "langgraph_cpp_blob_store_concurrency_test";
        fs::remove_all(root);

        lc::FileSystemBlobStore storeA(root);
        lc::FileSystemBlobStore storeB(root);
        std::atomic<int> failures { 0 };
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&storeA, &storeB, &failures, worker] {
                auto& store = (worker % 2 == 0) ? storeA : storeB;
                for (int i = 0; i < 30; ++i) {
                    const lc::BlobKey key {
                        .namespace_ = "worker-" + std::to_string(worker),
                        .name_ = "blob-" + std::to_string(i),
                    };
                    const auto value = "value-" + std::to_string(worker) + "-" + std::to_string(i);
                    if (!store.put(key, bytesFromText(value)).isOk()) {
                        ++failures;
                        continue;
                    }
                    auto loaded = store.get(key);
                    if (!loaded.isOk() || !loaded->has_value() || textFromBytes((*loaded)->data_) != value)
                        ++failures;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(failures.load() == 0);
        assert(storeA.list().isOk());

        fs::remove_all(root);
    }

    return 0;
}
