#pragma once

#include "foundation/blob/blob_store.hpp"
#include "foundation/filesystem/filesystem.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lgc::blob_detail {

namespace fs = std::filesystem;

inline constexpr std::string_view kBlobStoreMarkerName = ".langgraph-blob-store";
inline constexpr std::string_view kBlobStoreMarkerContents = "langgraph-cpp blob store\n";
inline constexpr std::string_view kBlobStoreLockName = ".langgraph-blob-store.lock";
inline constexpr std::size_t kSha256HexLength = 64;

struct StagedContent {
    TempFile temp_;
    std::size_t size_ { 0 };
    std::string checksumSha256_;
};

[[nodiscard]] bool matches(const BlobKey& key, const BlobListOptions& options);
[[nodiscard]] std::pair<std::string, std::string> toOrderedKey(const BlobKey& key);
[[nodiscard]] fs::path lockPath(const fs::path& rootDirectory);
[[nodiscard]] Status validateStoreMarkerFile(const fs::path& marker);
[[nodiscard]] bool isSha256Hex(std::string_view value) noexcept;
[[nodiscard]] Result<std::string> sha256Hex(std::span<const std::byte> data);
[[nodiscard]] Status requireBlobWithinLimit(std::size_t size, const BlobStoreOptions& options);
[[nodiscard]] Status validateBlobListOptionsForStore(
    const BlobListOptions& listOptions,
    const BlobStoreOptions& storeOptions);
[[nodiscard]] Result<bool> initializedStore(const fs::path& rootDirectory);
[[nodiscard]] std::chrono::system_clock::time_point nowSystem();
[[nodiscard]] std::int64_t toUnixMillis(std::chrono::system_clock::time_point time);
[[nodiscard]] std::chrono::system_clock::time_point fromUnixMillis(std::int64_t value);
[[nodiscard]] bool isMetadataFile(const fs::path& path);
[[nodiscard]] bool isBlobTempFile(const fs::path& path);
[[nodiscard]] bool isBlobContentFile(const fs::path& path);
[[nodiscard]] Result<BlobData> readBytes(const fs::path& path, const BlobStoreOptions& options);
[[nodiscard]] Result<void> streamBytes(
    const fs::path& path,
    const BlobInfo& info,
    const BlobStoreOptions& storeOptions,
    const BlobReadCallback& callback,
    const BlobReadOptions& readOptions);
[[nodiscard]] Result<StagedContent> stageContentFromFile(
    const fs::path& sourcePath,
    const fs::path& stagingDirectory,
    const BlobStoreOptions& options);
[[nodiscard]] Result<bool> installStagedContent(
    TempFile& temp,
    const fs::path& contentPath,
    std::size_t expectedSize);
[[nodiscard]] Result<void> writeDataFile(const fs::path& path, std::span<const std::byte> data);
[[nodiscard]] Status removeFileStrict(const fs::path& path, std::string_view label);

} // namespace lgc::blob_detail
