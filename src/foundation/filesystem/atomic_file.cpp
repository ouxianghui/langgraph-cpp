#include "foundation/filesystem/filesystem.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lgc {
namespace fs = std::filesystem;
namespace {

[[nodiscard]] Result<void> syncDirectory(const fs::path& directory)
{
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(
        directory.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return Status::internal("failed to open directory for flush");

    const bool ok = ::FlushFileBuffers(handle) != 0;
    ::CloseHandle(handle);
    if (!ok)
        return Status::internal("failed to flush directory");
    return okResult();
#else
    const int fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Status::internal("failed to open directory for flush: " + std::string(std::strerror(errno)));

    while (::fsync(fd) != 0) {
        if (errno == EINTR)
            continue;
        const int saved = errno;
        (void)::close(fd);
        return Status::internal("failed to flush directory: " + std::string(std::strerror(saved)));
    }

    if (::close(fd) != 0)
        return Status::internal("failed to close directory after flush: " + std::string(std::strerror(errno)));
    return okResult();
#endif
}

[[nodiscard]] Result<void> installTempFile(
    const fs::path& tempPath,
    const fs::path& targetPath,
    bool replaceExisting)
{
#if defined(_WIN32)
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replaceExisting)
        flags |= MOVEFILE_REPLACE_EXISTING;
    if (::MoveFileExW(tempPath.c_str(), targetPath.c_str(), flags))
        return okResult();

    const auto error = ::GetLastError();
    if (!replaceExisting && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
        return Status::alreadyExists("target file already exists");
    return Status::internal("failed to install temporary file");
#else
    if (!replaceExisting) {
        if (::link(tempPath.c_str(), targetPath.c_str()) == 0) {
            std::error_code ec;
            fs::remove(tempPath, ec);
            return okResult();
        }
        if (errno == EEXIST)
            return Status::alreadyExists("target file already exists");
        return Status::internal("failed to install temporary file: " + std::string(std::strerror(errno)));
    }

    std::error_code ec;
    fs::rename(tempPath, targetPath, ec);
    if (!ec)
        return okResult();
    return Status::internal("failed to atomically replace file: " + ec.message());
#endif
}

[[nodiscard]] Result<TempFile> makeAtomicTempFile(const fs::path& target, const AtomicWriteOptions& options)
{
    TempFileOptions tempOptions;
    tempOptions.directory_ = target.parent_path();
    tempOptions.prefix_ = options.tempPrefix_ + target.filename().string() + "-";
    tempOptions.suffix_ = ".tmp";
    tempOptions.createDirectory_ = false;
    tempOptions.pathPolicy_ = options.pathPolicy_;

    return TempFile::create(tempOptions);
}

} // namespace

Result<void> writeFileAtomic(
    const std::filesystem::path& path,
    std::string_view data,
    const AtomicWriteOptions& options)
{
    return writeFileAtomic(path, std::as_bytes(std::span(data.data(), data.size())), options);
}

Result<void> writeFileAtomic(
    const std::filesystem::path& path,
    std::span<const std::byte> data,
    const AtomicWriteOptions& options)
{
    auto normalized = normalize(path);
    if (!normalized.isOk())
        return normalized.status();

    const auto parent = normalized->parent_path();
    if (parent.empty())
        return Status::invalidArgument("target path must have a parent directory");

    if (options.createParentDirectories_) {
        if (auto result = ensureDir(parent); !result.isOk())
            return result.status();
    }

    auto temp = makeAtomicTempFile(*normalized, options);
    if (!temp.isOk())
        return temp.status();

    if (auto result = temp->write(data); !result.isOk())
        return result.status();

    if (options.durable_) {
        if (auto result = temp->flush(); !result.isOk())
            return result.status();
    }

    if (auto result = temp->close(); !result.isOk())
        return result.status();

    const auto tempPath = temp->path();
    auto installed = installTempFile(tempPath, *normalized, options.replaceExisting_);
    if (!installed.isOk()) {
        (void)temp->remove();
        return installed.status();
    }

    (void)temp->release();

    if (options.durable_) {
        if (auto result = syncDirectory(parent); !result.isOk())
            return result.status();
    }

    return okResult();
}

} // namespace lgc
