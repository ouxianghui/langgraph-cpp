#include "foundation/filesystem/filesystem.hpp"

#include "foundation/filesystem/filesystem_common.hh"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

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

class TempFileHandle final {
public:
#if defined(_WIN32)
    explicit TempFileHandle(HANDLE handle)
        : handle_(handle)
    {
    }

    ~TempFileHandle() { (void)close(); }

    TempFileHandle(const TempFileHandle&) = delete;
    TempFileHandle& operator=(const TempFileHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] Result<void> write(std::span<const std::byte> data) const
    {
        if (!valid())
            return Status::failedPrecondition("temporary file handle is not valid");

        LARGE_INTEGER offset {};
        if (::SetFilePointerEx(handle_, offset, nullptr, FILE_BEGIN) == 0)
            return Status::internal("failed to seek temporary file");
        if (::SetEndOfFile(handle_) == 0)
            return Status::internal("failed to truncate temporary file");

        const auto* current = reinterpret_cast<const unsigned char*>(data.data());
        std::size_t remaining = data.size();
        while (remaining != 0U) {
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (::WriteFile(handle_, current, chunk, &written, nullptr) == 0)
                return Status::internal("failed to write temporary file");
            if (written == 0U)
                return Status::internal("temporary file write made no progress");
            current += written;
            remaining -= written;
        }

        return okResult();
    }

    [[nodiscard]] Result<void> flush() const
    {
        if (!valid())
            return Status::failedPrecondition("temporary file handle is not valid");
        if (::FlushFileBuffers(handle_) == 0)
            return Status::internal("failed to flush temporary file");
        return okResult();
    }

    [[nodiscard]] Result<void> close() noexcept
    {
        if (!valid())
            return okResult();
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        if (::CloseHandle(handle) == 0)
            return Status::internal("failed to close temporary file");
        return okResult();
    }

private:
    HANDLE handle_ { INVALID_HANDLE_VALUE };
#else
    explicit TempFileHandle(int fd)
        : fd_(fd)
    {
    }

    ~TempFileHandle() { (void)close(); }

    TempFileHandle(const TempFileHandle&) = delete;
    TempFileHandle& operator=(const TempFileHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    [[nodiscard]] Result<void> write(std::span<const std::byte> data) const
    {
        if (!valid())
            return Status::failedPrecondition("temporary file handle is not valid");

        if (data.size() > static_cast<std::size_t>(std::numeric_limits<off_t>::max()))
            return Status::outOfRange("temporary file is too large");

        if (::ftruncate(fd_, 0) != 0)
            return Status::internal("failed to truncate temporary file: " + std::string(std::strerror(errno)));
        if (::lseek(fd_, 0, SEEK_SET) < 0)
            return Status::internal("failed to seek temporary file: " + std::string(std::strerror(errno)));

        const auto* current = reinterpret_cast<const unsigned char*>(data.data());
        std::size_t remaining = data.size();
        while (remaining != 0U) {
            const auto chunk = std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t written = ::write(fd_, current, chunk);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                return Status::internal("failed to write temporary file: " + std::string(std::strerror(errno)));
            }
            if (written == 0)
                return Status::internal("temporary file write made no progress");
            current += written;
            remaining -= static_cast<std::size_t>(written);
        }

        return okResult();
    }

    [[nodiscard]] Result<void> flush() const
    {
        if (!valid())
            return Status::failedPrecondition("temporary file handle is not valid");

        while (::fsync(fd_) != 0) {
            if (errno == EINTR)
                continue;
            return Status::internal("failed to flush temporary file: " + std::string(std::strerror(errno)));
        }
        return okResult();
    }

    [[nodiscard]] Result<void> close() noexcept
    {
        if (!valid())
            return okResult();
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0)
            return Status::internal("failed to close temporary file: " + std::string(std::strerror(errno)));
        return okResult();
    }

private:
    int fd_ { -1 };
#endif
};

namespace {
using filesystem_detail::containsPathSeparator;
using filesystem_detail::validatePathComponent;

[[nodiscard]] fs::path defaultTempDirectory()
{
    std::error_code ec;
    auto path = fs::temp_directory_path(ec);
    if (ec)
        return fs::current_path(ec);
    return path;
}

[[nodiscard]] Status validateTempAffix(
    std::string_view value,
    std::string_view field,
    const PathPolicy& policy)
{
    if (containsPathSeparator(value)) {
        std::string message("temporary file ");
        message.append(field);
        message.append(" cannot contain path separators");
        return Status::invalidArgument(std::move(message));
    }

    if (value.empty())
        return Status::ok();

    return validatePathComponent(value, field, policy);
}

[[nodiscard]] std::string uniqueName(std::string_view prefix, std::string_view suffix)
{
    static std::atomic<std::uint64_t> counter { 1 };
    static thread_local std::mt19937_64 rng(std::random_device {}());

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tid = std::hash<std::thread::id> {}(std::this_thread::get_id());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream out;
    out << prefix << std::hex << now << "-" << tid << "-" << seq << "-" << rng() << suffix;
    return out.str();
}

[[nodiscard]] Result<std::unique_ptr<TempFileHandle>> createFileExclusive(const fs::path& path)
{
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            return Status::alreadyExists("temporary file already exists");
        return Status::unavailable("failed to create temporary file exclusively");
    }
    return std::make_unique<TempFileHandle>(handle);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            return Status::alreadyExists("temporary file already exists");
        return Status::unavailable("failed to create temporary file exclusively: " + std::string(std::strerror(errno)));
    }
    return std::make_unique<TempFileHandle>(fd);
#endif
}

} // namespace

TempFile::TempFile() = default;

TempFile::TempFile(std::filesystem::path path, std::unique_ptr<TempFileHandle> handle)
    : path_(std::move(path))
    , handle_(std::move(handle))
    , removeOnDestroy_(true)
{
}

TempFile::~TempFile()
{
    (void)close();
    if (removeOnDestroy_)
        (void)remove();
}

TempFile::TempFile(TempFile&& other) noexcept
    : path_(std::move(other.path_))
    , handle_(std::move(other.handle_))
    , removeOnDestroy_(other.removeOnDestroy_)
{
    other.removeOnDestroy_ = false;
}

TempFile& TempFile::operator=(TempFile&& other) noexcept
{
    if (this == &other)
        return *this;

    if (removeOnDestroy_)
        (void)remove();

    path_ = std::move(other.path_);
    handle_ = std::move(other.handle_);
    removeOnDestroy_ = other.removeOnDestroy_;
    other.removeOnDestroy_ = false;
    return *this;
}

Result<TempFile> TempFile::create(const TempFileOptions& options)
{
    if (auto status = validateTempAffix(options.prefix_, "prefix", options.pathPolicy_); !status.isOk())
        return status;
    if (auto status = validateTempAffix(options.suffix_, "suffix", options.pathPolicy_); !status.isOk())
        return status;

    fs::path directory = options.directory_.empty() ? defaultTempDirectory() : options.directory_;
    if (options.createDirectory_) {
        if (auto result = ensureDir(directory); !result.isOk())
            return result.status();
    }

    auto normalizedDirectory = normalize(directory);
    if (!normalizedDirectory.isOk())
        return normalizedDirectory.status();

    for (int i = 0; i < 128; ++i) {
        const auto name = uniqueName(options.prefix_, options.suffix_);
        if (auto status = validatePathComponent(name, "temporary_file_name", options.pathPolicy_); !status.isOk())
            return status;

        auto candidate = *normalizedDirectory / name;
        auto created = createFileExclusive(candidate);
        if (!created.isOk()) {
            if (created.status().code() == StatusCode::AlreadyExists)
                continue;
            return created.status();
        }
        return TempFile(candidate, std::move(*created));
    }

    return Status::resourceExhausted("failed to create a unique temporary file");
}

const std::filesystem::path& TempFile::path() const noexcept
{
    return path_;
}

bool TempFile::valid() const noexcept
{
    return !path_.empty() && handle_ && handle_->valid();
}

Result<void> TempFile::write(std::string_view data) const
{
    return write(std::as_bytes(std::span(data.data(), data.size())));
}

Result<void> TempFile::write(std::span<const std::byte> data) const
{
    if (!valid())
        return Status::failedPrecondition("temporary file is not valid");
    return handle_->write(data);
}

Result<void> TempFile::writeBytes(std::span<const std::byte> data) const
{
    return write(data);
}

Result<void> TempFile::flush() const
{
    if (!valid())
        return Status::failedPrecondition("temporary file is not valid");
    return handle_->flush();
}

Result<void> TempFile::close() noexcept
{
    if (!handle_)
        return okResult();

    auto status = handle_->close();
    handle_.reset();
    return status;
}

Result<void> TempFile::remove() noexcept
{
    if (path_.empty()) {
        removeOnDestroy_ = false;
        return okResult();
    }

    auto closeStatus = close();

    std::error_code ec;
    fs::remove(path_, ec);
    if (ec) {
        std::string message("failed to remove temporary file: ");
        message.append(ec.message());
        return Status::internal(std::move(message));
    }

    removeOnDestroy_ = false;
    path_.clear();
    if (!closeStatus.isOk())
        return closeStatus;
    return okResult();
}

std::filesystem::path TempFile::release() noexcept
{
    (void)close();
    removeOnDestroy_ = false;
    return std::move(path_);
}

} // namespace lgc
