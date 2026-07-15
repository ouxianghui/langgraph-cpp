#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lgc {

struct PathPolicy {
    std::size_t maxPathLength_ { 4096 };
    std::size_t maxComponentLength_ { 255 };
    std::string allowedChars_ { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" };
    bool rejectReservedNames_ { true };
};

struct AtomicWriteOptions {
    bool createParentDirectories_ { true };
    bool replaceExisting_ { true };
    bool durable_ { false };
    std::string tempPrefix_ { ".tmp-" };
    PathPolicy pathPolicy_;
};

struct TempFileOptions {
    std::filesystem::path directory_;
    std::string prefix_ { "langgraph-cpp-" };
    std::string suffix_ { ".tmp" };
    bool createDirectory_ { true };
    PathPolicy pathPolicy_;
};

struct ReadFileOptions {
    /// `0` means unbounded.
    std::size_t maxBytes_ { 64 * 1024 * 1024 };
};

class TempFileHandle;

class TempFile final {
public:
    TempFile();
    ~TempFile();

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    TempFile(TempFile&& other) noexcept;
    TempFile& operator=(TempFile&& other) noexcept;

    [[nodiscard]] static Result<TempFile> create(const TempFileOptions& options = {});

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] Result<void> write(std::string_view data) const;
    [[nodiscard]] Result<void> write(std::span<const std::byte> data) const;
    [[nodiscard]] Result<void> writeBytes(std::span<const std::byte> data) const;
    [[nodiscard]] Result<void> flush() const;
    [[nodiscard]] Result<void> close() noexcept;
    [[nodiscard]] Result<void> remove() noexcept;

    /// Prevent destructor cleanup and return the path to the caller.
    [[nodiscard]] std::filesystem::path release() noexcept;

private:
    explicit TempFile(std::filesystem::path path, std::unique_ptr<TempFileHandle> handle);

    std::filesystem::path path_;
    mutable std::unique_ptr<TempFileHandle> handle_;
    bool removeOnDestroy_ { false };
};

[[nodiscard]] Result<std::filesystem::path> normalize(
    const std::filesystem::path& path,
    const std::filesystem::path& baseDirectory = {});

[[nodiscard]] Result<std::filesystem::path> realPath(
    const std::filesystem::path& path,
    const std::filesystem::path& baseDirectory = {});

[[nodiscard]] Result<bool> isInside(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate);

[[nodiscard]] Status requireInside(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate);

[[nodiscard]] Status requireSafeRelativePath(
    const std::filesystem::path& relativePath,
    const PathPolicy& policy = {});

[[nodiscard]] Result<std::filesystem::path> resolveChild(
    const std::filesystem::path& directory,
    const std::filesystem::path& relativePath,
    const PathPolicy& policy = {});

[[nodiscard]] Result<void> ensureDir(const std::filesystem::path& directory);

[[nodiscard]] Result<void> writeFileAtomic(
    const std::filesystem::path& path,
    std::string_view data,
    const AtomicWriteOptions& options = {});

[[nodiscard]] Result<void> writeFileAtomic(
    const std::filesystem::path& path,
    std::span<const std::byte> data,
    const AtomicWriteOptions& options = {});

[[nodiscard]] Result<std::string> readFile(
    const std::filesystem::path& path,
    const ReadFileOptions& options = {});

} // namespace lgc
