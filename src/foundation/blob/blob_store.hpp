#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/status/result.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

using BlobData = std::vector<std::byte>;
using BlobMetadata = std::map<std::string, std::string>;

struct BlobKey {
    std::string namespace_;
    std::string name_;

    friend bool operator==(const BlobKey&, const BlobKey&) = default;
};

struct BlobInfo {
    BlobKey key_;
    std::string contentType_;
    std::size_t size_ { 0 };
    std::string checksumSha256_;
    BlobMetadata metadata_;
    std::chrono::system_clock::time_point createdAt_;
    std::chrono::system_clock::time_point updatedAt_;
};

struct Blob {
    BlobInfo info_;
    BlobData data_;
};

struct BlobPutOptions {
    std::string contentType_ { "application/octet-stream" };
    BlobMetadata metadata_;
    bool replace_ { true };
};

struct BlobListOptions {
    std::string namespace_;
    std::string namePrefix_;
    std::size_t limit_ { 100 };
    std::string cursor_;
};

struct BlobListResult {
    std::vector<BlobInfo> items_;
    std::string nextCursor_;
};

struct BlobReadOptions {
    std::size_t chunkBytes_ { 64 * 1024 };
};

struct BlobStoreOptions {
    std::optional<std::size_t> maxBlobBytes_;
    std::size_t maxMetadataBytes_ { 128 * 1024 };
    std::size_t maxMetadataEntries_ { 128 };
    std::size_t maxMetadataKeyBytes_ { 128 };
    std::size_t maxMetadataValueBytes_ { 4096 };
    std::size_t maxContentTypeBytes_ { 256 };
    std::size_t maxListItems_ { 1000 };
    std::size_t maxListScanEntries_ { 100'000 };
};

using BlobReadCallback = std::function<Status(std::span<const std::byte>)>;

class IBlobStore {
public:
    virtual ~IBlobStore() = default;

    [[nodiscard]] virtual Result<void> put(
        BlobKey key,
        BlobData data,
        const BlobPutOptions& options = {}) = 0;
    [[nodiscard]] virtual Result<void> putFile(
        BlobKey key,
        const std::filesystem::path& sourcePath,
        const BlobPutOptions& options = {}) = 0;
    [[nodiscard]] virtual Result<std::optional<BlobInfo>> stat(const BlobKey& key) = 0;
    [[nodiscard]] virtual Result<std::optional<Blob>> get(const BlobKey& key) = 0;
    [[nodiscard]] virtual Result<void> read(
        const BlobKey& key,
        const BlobReadCallback& callback,
        const BlobReadOptions& options = {}) = 0;
    [[nodiscard]] virtual Result<BlobListResult> list(const BlobListOptions& options = {}) = 0;
    [[nodiscard]] virtual Result<void> remove(const BlobKey& key) = 0;
    [[nodiscard]] virtual Result<void> clearNamespace(std::string_view namespaceName) = 0;
    [[nodiscard]] virtual Result<void> compact() = 0;
};

class MemoryBlobStore final : public IBlobStore {
public:
    explicit MemoryBlobStore(BlobStoreOptions options = {});

    [[nodiscard]] Result<void> put(
        BlobKey key,
        BlobData data,
        const BlobPutOptions& options = {}) override;
    [[nodiscard]] Result<void> putFile(
        BlobKey key,
        const std::filesystem::path& sourcePath,
        const BlobPutOptions& options = {}) override;
    [[nodiscard]] Result<std::optional<BlobInfo>> stat(const BlobKey& key) override;
    [[nodiscard]] Result<std::optional<Blob>> get(const BlobKey& key) override;
    [[nodiscard]] Result<void> read(
        const BlobKey& key,
        const BlobReadCallback& callback,
        const BlobReadOptions& options = {}) override;
    [[nodiscard]] Result<BlobListResult> list(const BlobListOptions& options = {}) override;
    [[nodiscard]] Result<void> remove(const BlobKey& key) override;
    [[nodiscard]] Result<void> clearNamespace(std::string_view namespaceName) override;
    [[nodiscard]] Result<void> compact() override;

private:
    using MapKey = std::pair<std::string, std::string>;

    [[nodiscard]] static MapKey toMapKey(const BlobKey& key);

    std::mutex mutex_;
    std::map<MapKey, Blob> blobs_;
    BlobStoreOptions options_;
};

class FileSystemBlobStore final : public IBlobStore {
public:
    explicit FileSystemBlobStore(
        std::filesystem::path rootDirectory,
        BlobStoreOptions options = {},
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    [[nodiscard]] Result<void> put(
        BlobKey key,
        BlobData data,
        const BlobPutOptions& options = {}) override;
    [[nodiscard]] Result<void> putFile(
        BlobKey key,
        const std::filesystem::path& sourcePath,
        const BlobPutOptions& options = {}) override;
    [[nodiscard]] Result<std::optional<BlobInfo>> stat(const BlobKey& key) override;
    [[nodiscard]] Result<std::optional<Blob>> get(const BlobKey& key) override;
    [[nodiscard]] Result<void> read(
        const BlobKey& key,
        const BlobReadCallback& callback,
        const BlobReadOptions& options = {}) override;
    [[nodiscard]] Result<BlobListResult> list(const BlobListOptions& options = {}) override;
    [[nodiscard]] Result<void> remove(const BlobKey& key) override;
    [[nodiscard]] Result<void> clearNamespace(std::string_view namespaceName) override;
    [[nodiscard]] Result<void> compact() override;

    [[nodiscard]] const std::filesystem::path& rootDirectory() const noexcept;

private:
    [[nodiscard]] std::filesystem::path markerPath() const;
    [[nodiscard]] Result<std::filesystem::path> namespaceDirectory(std::string_view namespaceName) const;
    [[nodiscard]] Result<std::filesystem::path> contentPath(const BlobInfo& info) const;
    [[nodiscard]] Result<std::filesystem::path> metadataPath(const BlobKey& key) const;
    [[nodiscard]] Status ensureStoreDirectory() const;
    [[nodiscard]] Status requireStoreMarker() const;
    [[nodiscard]] Status requireMetadataPathMatches(
        const BlobInfo& info,
        const std::filesystem::path& metadataFile) const;
    [[nodiscard]] Result<BlobInfo> readInfo(const BlobKey& key) const;

    std::filesystem::path rootDirectory_;
    BlobStoreOptions options_;
    std::shared_ptr<ILogger> logger_;
    std::mutex mutex_;
};

[[nodiscard]] Status validateBlobKey(const BlobKey& key);
[[nodiscard]] Status validateBlobListOptions(const BlobListOptions& options);
[[nodiscard]] Status validateBlobReadOptions(const BlobReadOptions& options);
[[nodiscard]] std::string encodeBlobCursor(const BlobKey& key);
[[nodiscard]] Result<std::optional<BlobKey>> decodeBlobCursor(std::string_view cursor);

} // namespace lc
