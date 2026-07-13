#include "foundation/process/process_runner_common.hh"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <thread>
#include <windows.h>
#include <utility>

namespace lc::process_runner_detail {
namespace {

using process_detail::appendBounded;
using process_detail::hasStreamingStdin;
using process_detail::lowerAscii;
using process_detail::statusFromExitCode;
using process_detail::streamProcessStdin;

class Handle final {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE handle) noexcept
        : handle_(handle)
    {
    }

    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    Handle& operator=(Handle&& other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        handle_ = std::exchange(other.handle_, nullptr);
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE handle = nullptr) noexcept
    {
        if (valid())
            (void)::CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ { nullptr };
};

[[nodiscard]] std::string quoteArg(std::string_view arg)
{
    if (arg.empty())
        return "\"\"";
    if (arg.find_first_of(" \t\n\v\"") == std::string_view::npos)
        return std::string(arg);

    std::string out;
    out.push_back('"');
    std::size_t backslashes = 0;
    for (const auto ch : arg) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string buildCommandLine(const ProcessOptions& options)
{
    std::string out = quoteArg(options.executable_);
    for (const auto& arg : options.arguments_) {
        out.push_back(' ');
        out.append(quoteArg(arg));
    }
    return out;
}

void drainAvailable(HANDLE handle, std::string& output, bool& truncated, std::size_t limit) noexcept
{
    DWORD available = 0;
    while (::PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        char buffer[4096];
        DWORD read = 0;
        const auto toRead = std::min<DWORD>(available, sizeof(buffer));
        if (!::ReadFile(handle, buffer, toRead, &read, nullptr) || read == 0)
            return;
        appendBounded(output, truncated, buffer, read, limit);
        available = 0;
    }
}

[[nodiscard]] Status writeAllToHandle(HANDLE handle, std::string_view data) noexcept
{
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 64 * 1024));
        DWORD written = 0;
        if (!::WriteFile(handle, cursor, chunk, &written, nullptr)) {
            const auto error = ::GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
                return Status::ok();
            return Status::internal("failed to write process stdin");
        }
        cursor += written;
        remaining -= written;
        if (written == 0)
            return Status::internal("failed to write process stdin");
    }
    return Status::ok();
}

[[nodiscard]] Status writeStreamingStdin(Handle writeHandle, const ProcessOptions& options)
{
    return streamProcessStdin(options, [&](std::string_view chunk) {
        return writeAllToHandle(writeHandle.get(), chunk);
    });
}

[[nodiscard]] Result<Handle> openWindowsNullStdin(const ProcessOptions& options, SECURITY_ATTRIBUTES& security)
{
    if (options.inheritStdin_)
        return Handle();

    HANDLE handle = ::CreateFileA(
        "NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return Status::internal("failed to open NUL for process stdin");
    return Handle(handle);
}

struct CaseInsensitiveLess {
    [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const
    {
        return lowerAscii(lhs) < lowerAscii(rhs);
    }
};

[[nodiscard]] Result<std::vector<char>> buildEnvironmentBlock(const ProcessOptions& options)
{
    std::map<std::string, std::string, CaseInsensitiveLess> entries;
    if (options.inheritEnvironment_) {
        LPCH env = ::GetEnvironmentStringsA();
        if (!env)
            return Status::internal("failed to read process environment");
        for (LPCH it = env; *it != '\0';) {
            const auto len = std::strlen(it);
            std::string_view entry(it, len);
            const auto separator = entry.find('=');
            if (separator != std::string_view::npos && separator != 0) {
                const std::string name(entry.substr(0, separator));
                entries[name] = std::string(entry);
            }
            it += len + 1;
        }
        ::FreeEnvironmentStringsA(env);
    }

    for (const auto& [name, value] : options.environment_)
        entries[name] = name + "=" + value;

    std::vector<char> block;
    for (const auto& [_, entry] : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}

} // namespace

Result<ProcessResult> runPlatformProcess(
    const ProcessOptions& options,
    const Clock& clock)
{
    SECURITY_ATTRIBUTES security {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!::CreatePipe(&stdoutRead, &stdoutWrite, &security, 0))
        return errorResult<ProcessResult>(Status::internal("CreatePipe failed"));
    if (!::CreatePipe(&stderrRead, &stderrWrite, &security, 0)) {
        (void)::CloseHandle(stdoutRead);
        (void)::CloseHandle(stdoutWrite);
        return errorResult<ProcessResult>(Status::internal("CreatePipe failed"));
    }

    Handle outRead(stdoutRead);
    Handle outWrite(stdoutWrite);
    Handle errRead(stderrRead);
    Handle errWrite(stderrWrite);
    (void)::SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    (void)::SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);

    Handle stdinRead;
    Handle stdinWrite;
    if (hasStreamingStdin(options)) {
        HANDLE read = nullptr;
        HANDLE write = nullptr;
        if (!::CreatePipe(&read, &write, &security, 0))
            return errorResult<ProcessResult>(Status::internal("CreatePipe failed"));
        stdinRead = Handle(read);
        stdinWrite = Handle(write);
        (void)::SetHandleInformation(stdinWrite.get(), HANDLE_FLAG_INHERIT, 0);
    } else {
        auto nullHandle = openWindowsNullStdin(options, security);
        if (!nullHandle.isOk())
            return errorResult<ProcessResult>(nullHandle.status());
        stdinRead = std::move(*nullHandle);
    }
    HANDLE childStdin = stdinRead.valid() ? stdinRead.get() : ::GetStdHandle(STD_INPUT_HANDLE);

    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = outWrite.get();
    startup.hStdError = errWrite.get();
    startup.hStdInput = childStdin;

    PROCESS_INFORMATION processInfo {};
    auto cmd = buildCommandLine(options);
    const auto cwd = options.workingDirectory_ ? options.workingDirectory_->string() : std::string();

    std::vector<char> envBlock;
    LPVOID environment = nullptr;
    if (!options.inheritEnvironment_ || !options.environment_.empty()) {
        auto block = buildEnvironmentBlock(options);
        if (!block.isOk())
            return errorResult<ProcessResult>(block.status());
        envBlock = std::move(*block);
        environment = envBlock.data();
    }

    const BOOL created = ::CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        environment,
        cwd.empty() ? nullptr : cwd.c_str(),
        &startup,
        &processInfo);

    outWrite.reset();
    errWrite.reset();

    if (!created)
        return errorResult<ProcessResult>(Status::notFound("failed to execute process"));

    stdinRead.reset();
    Status stdinStatus = Status::ok();
    std::thread stdinThread;
    if (stdinWrite.valid()) {
        Handle writeHandle = std::move(stdinWrite);
        stdinThread = std::thread([&stdinStatus, &options, writeHandle = std::move(writeHandle)]() mutable {
            stdinStatus = writeStreamingStdin(std::move(writeHandle), options);
        });
    }
    auto joinStdin = [&]() -> Status {
        if (stdinThread.joinable())
            stdinThread.join();
        return stdinStatus;
    };

    Handle processHandle(processInfo.hProcess);
    Handle threadHandle(processInfo.hThread);
    Handle jobHandle(options.terminateProcessTree_ ? ::CreateJobObjectA(nullptr, nullptr) : nullptr);
    if (options.terminateProcessTree_ && jobHandle.valid()) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        (void)::SetInformationJobObject(
            jobHandle.get(),
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits));
        (void)::AssignProcessToJobObject(jobHandle.get(), processHandle.get());
    }

    ProcessResult result;
    const auto startedAt = clock.now();
    const auto deadline = options.timeout_ ? std::optional<Clock::TimePoint>(startedAt + *options.timeout_) : std::nullopt;

    while (true) {
        drainAvailable(outRead.get(), result.stdout_, result.stdoutTruncated_, options.maxStdoutBytes_);
        drainAvailable(errRead.get(), result.stderr_, result.stderrTruncated_, options.maxStderrBytes_);

        const auto wait = ::WaitForSingleObject(processHandle.get(), 5);
        if (wait == WAIT_OBJECT_0)
            break;

        if (options.cancellation_.cancelled()) {
            result.cancelled_ = true;
            if (jobHandle.valid())
                (void)::TerminateJobObject(jobHandle.get(), 1);
            else
                (void)::TerminateProcess(processHandle.get(), 1);
            (void)::WaitForSingleObject(processHandle.get(), INFINITE);
            break;
        }
        if (deadline && clock.now() >= *deadline) {
            result.timedOut_ = true;
            if (jobHandle.valid())
                (void)::TerminateJobObject(jobHandle.get(), 1);
            else
                (void)::TerminateProcess(processHandle.get(), 1);
            (void)::WaitForSingleObject(processHandle.get(), INFINITE);
            break;
        }
    }

    drainAvailable(outRead.get(), result.stdout_, result.stdoutTruncated_, options.maxStdoutBytes_);
    drainAvailable(errRead.get(), result.stderr_, result.stderrTruncated_, options.maxStderrBytes_);

    const auto stdinWriteStatus = joinStdin();

    if (result.cancelled_) {
        result.status_ = Status::cancelled(options.cancellation_.reason().empty() ? "process cancelled" : options.cancellation_.reason());
        return okResult(std::move(result));
    }
    if (result.timedOut_) {
        result.status_ = Status::deadlineExceeded("process timed out");
        return okResult(std::move(result));
    }
    if (!stdinWriteStatus.isOk())
        return errorResult<ProcessResult>(stdinWriteStatus);

    DWORD exitCode = 0;
    if (!::GetExitCodeProcess(processHandle.get(), &exitCode))
        return errorResult<ProcessResult>(Status::internal("GetExitCodeProcess failed"));

    result.exited_ = true;
    result.exitCode_ = static_cast<int>(exitCode);
    result.status_ = statusFromExitCode(result.exitCode_);
    return okResult(std::move(result));
}

} // namespace lc::process_runner_detail

#endif
