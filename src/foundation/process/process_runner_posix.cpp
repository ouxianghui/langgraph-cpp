#include "foundation/process/process_runner_common.hh"

#if !defined(_WIN32)

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace lc::process_runner_detail {
namespace {

using namespace std::chrono_literals;
using process_detail::appendBounded;
using process_detail::hasStreamingStdin;
using process_detail::statusFromErrno;
using process_detail::statusFromExitCode;
using process_detail::streamProcessStdin;

class Fd final {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept
        : fd_(fd)
    {
    }

    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {
    }

    Fd& operator=(Fd&& other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        fd_ = std::exchange(other.fd_, -1);
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            (void)::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ { -1 };
};

[[nodiscard]] Result<std::pair<Fd, Fd>> makePipe()
{
    int fds[2] { -1, -1 };
    if (::pipe(fds) != 0)
        return errorResult<std::pair<Fd, Fd>>(Status::internal("pipe failed: " + std::string(std::strerror(errno))));
    return okResult(std::pair<Fd, Fd>(Fd(fds[0]), Fd(fds[1])));
}

void setCloseOnExec(int fd) noexcept
{
    const auto flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void setNonBlocking(int fd) noexcept
{
    const auto flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void drainAvailable(int fd, std::string& output, bool& truncated, std::size_t limit, bool& open) noexcept
{
    char buffer[4096];
    while (open) {
        const auto count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            appendBounded(output, truncated, buffer, static_cast<std::size_t>(count), limit);
            continue;
        }
        if (count == 0) {
            open = false;
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        open = false;
    }
}

[[nodiscard]] Status writeAllToFd(int fd, std::string_view data) noexcept
{
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto written = ::write(fd, cursor, remaining);
        if (written > 0) {
            cursor += written;
            remaining -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && errno == EPIPE)
            return Status::ok();
        return statusFromErrno(errno, "failed to write process stdin");
    }
    return Status::ok();
}

[[nodiscard]] Status writeStreamingStdin(Fd writeFd, const ProcessOptions& options)
{
    return streamProcessStdin(options, [&](std::string_view chunk) {
        return writeAllToFd(writeFd.get(), chunk);
    });
}

[[nodiscard]] Result<Fd> openReadOnly(const std::filesystem::path& path)
{
    const int fd = ::open(path.string().c_str(), O_RDONLY);
    if (fd < 0)
        return statusFromErrno(errno, "failed to open process stdin");
    return Fd(fd);
}

void terminateProcess(pid_t pid, bool terminateProcessTree) noexcept
{
    if (pid <= 0)
        return;

    if (terminateProcessTree)
        (void)::kill(-pid, SIGTERM);
    (void)::kill(pid, SIGTERM);

    for (int i = 0; i < 10; ++i) {
        int status = 0;
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid || waited < 0)
            return;
        std::this_thread::sleep_for(10ms);
    }

    if (terminateProcessTree)
        (void)::kill(-pid, SIGKILL);
    (void)::kill(pid, SIGKILL);
}

} // namespace

Result<ProcessResult> runPlatformProcess(
    const ProcessOptions& options,
    const Clock& clock)
{
    auto stdoutPipe = makePipe();
    if (!stdoutPipe.isOk())
        return errorResult<ProcessResult>(stdoutPipe.status());
    auto stderrPipe = makePipe();
    if (!stderrPipe.isOk())
        return errorResult<ProcessResult>(stderrPipe.status());
    auto execPipe = makePipe();
    if (!execPipe.isOk())
        return errorResult<ProcessResult>(execPipe.status());

    std::optional<std::pair<Fd, Fd>> stdinPipe;
    Fd devNullStdin;
    int childStdinFd = -1;
    if (hasStreamingStdin(options)) {
        auto pipe = makePipe();
        if (!pipe.isOk())
            return errorResult<ProcessResult>(pipe.status());
        stdinPipe = std::move(*pipe);
        childStdinFd = stdinPipe->first.get();
    } else if (!options.inheritStdin_) {
        auto opened = openReadOnly("/dev/null");
        if (!opened.isOk())
            return errorResult<ProcessResult>(opened.status());
        devNullStdin = std::move(*opened);
        childStdinFd = devNullStdin.get();
    }

    setCloseOnExec(execPipe->second.get());

    const auto pid = ::fork();
    if (pid < 0)
        return errorResult<ProcessResult>(Status::internal("fork failed: " + std::string(std::strerror(errno))));

    if (pid == 0) {
        if (options.terminateProcessTree_)
            (void)::setpgid(0, 0);

        stdoutPipe->first.reset();
        stderrPipe->first.reset();
        execPipe->first.reset();
        if (stdinPipe)
            stdinPipe->second.reset();

        if (childStdinFd >= 0 && ::dup2(childStdinFd, STDIN_FILENO) < 0) {
            const int error = errno;
            (void)::write(execPipe->second.get(), &error, sizeof(error));
            _exit(127);
        }
        (void)::dup2(stdoutPipe->second.get(), STDOUT_FILENO);
        (void)::dup2(stderrPipe->second.get(), STDERR_FILENO);

        if (stdinPipe)
            stdinPipe->first.reset();
        devNullStdin.reset();
        stdoutPipe->second.reset();
        stderrPipe->second.reset();

        auto fail = [&](int error) {
            (void)::write(execPipe->second.get(), &error, sizeof(error));
            _exit(127);
        };

        if (options.workingDirectory_) {
            if (::chdir(options.workingDirectory_->string().c_str()) != 0)
                fail(errno);
        }

        if (!options.inheritEnvironment_)
            environ = nullptr;
        for (const auto& [name, value] : options.environment_) {
            if (::setenv(name.c_str(), value.c_str(), 1) != 0)
                fail(errno);
        }

        std::vector<char*> argv;
        argv.reserve(options.arguments_.size() + 2);
        argv.push_back(const_cast<char*>(options.executable_.c_str()));
        for (const auto& arg : options.arguments_)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        ::execvp(options.executable_.c_str(), argv.data());
        fail(errno);
    }

    if (options.terminateProcessTree_)
        (void)::setpgid(pid, pid);

    stdoutPipe->second.reset();
    stderrPipe->second.reset();
    execPipe->second.reset();
    if (stdinPipe)
        stdinPipe->first.reset();
    devNullStdin.reset();
    setNonBlocking(stdoutPipe->first.get());
    setNonBlocking(stderrPipe->first.get());
    setNonBlocking(execPipe->first.get());

    Status stdinStatus = Status::ok();
    std::thread stdinThread;
    if (stdinPipe) {
        Fd writeFd = std::move(stdinPipe->second);
        stdinThread = std::thread([&stdinStatus, &options, writeFd = std::move(writeFd)]() mutable {
            stdinStatus = writeStreamingStdin(std::move(writeFd), options);
        });
    }
    auto joinStdin = [&]() -> Status {
        if (stdinThread.joinable())
            stdinThread.join();
        return stdinStatus;
    };

    ProcessResult result;
    bool stdoutOpen = true;
    bool stderrOpen = true;
    bool execOpen = true;
    bool childExited = false;
    int waitStatus = 0;
    int execError = 0;
    const auto startedAt = clock.now();
    const auto deadline = options.timeout_ ? std::optional<Clock::TimePoint>(startedAt + *options.timeout_) : std::nullopt;

    auto readExecError = [&] {
        if (!execOpen)
            return;
        const auto count = ::read(execPipe->first.get(), &execError, sizeof(execError));
        if (count == 0)
            execOpen = false;
        else if (count > 0)
            execOpen = false;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            execOpen = false;
    };

    auto drainOpenPipes = [&] {
        drainAvailable(stdoutPipe->first.get(), result.stdout_, result.stdoutTruncated_, options.maxStdoutBytes_, stdoutOpen);
        drainAvailable(stderrPipe->first.get(), result.stderr_, result.stderrTruncated_, options.maxStderrBytes_, stderrOpen);
        readExecError();
    };

    while (stdoutOpen || stderrOpen || !childExited || execOpen) {
        drainOpenPipes();

        if (!childExited) {
            const auto waited = ::waitpid(pid, &waitStatus, WNOHANG);
            if (waited == pid) {
                childExited = true;
            } else if (waited < 0 && errno != EINTR) {
                (void)joinStdin();
                return errorResult<ProcessResult>(Status::internal("waitpid failed: " + std::string(std::strerror(errno))));
            }
        }

        if (!childExited) {
            if (options.cancellation_.cancelled()) {
                result.cancelled_ = true;
                terminateProcess(pid, options.terminateProcessTree_);
                (void)::waitpid(pid, &waitStatus, 0);
                childExited = true;
            } else if (deadline && clock.now() >= *deadline) {
                result.timedOut_ = true;
                terminateProcess(pid, options.terminateProcessTree_);
                (void)::waitpid(pid, &waitStatus, 0);
                childExited = true;
            }
        }

        if (childExited) {
            drainOpenPipes();
            stdoutOpen = false;
            stderrOpen = false;
            execOpen = false;
            break;
        }

        std::this_thread::sleep_for(5ms);
    }

    const auto stdinWriteStatus = joinStdin();

    if (execError != 0)
        return errorResult<ProcessResult>(statusFromErrno(execError, "failed to execute process"));

    if (!stdinWriteStatus.isOk() && !result.cancelled_ && !result.timedOut_)
        return errorResult<ProcessResult>(stdinWriteStatus);

    if (result.cancelled_) {
        result.status_ = Status::cancelled(options.cancellation_.reason().empty() ? "process cancelled" : options.cancellation_.reason());
        return okResult(std::move(result));
    }
    if (result.timedOut_) {
        result.status_ = Status::deadlineExceeded("process timed out");
        return okResult(std::move(result));
    }

    result.exited_ = true;
    if (WIFEXITED(waitStatus)) {
        result.exitCode_ = WEXITSTATUS(waitStatus);
        result.status_ = statusFromExitCode(result.exitCode_);
    } else if (WIFSIGNALED(waitStatus)) {
        result.exitCode_ = 128 + WTERMSIG(waitStatus);
        result.status_ = Status::aborted("process terminated by signal " + std::to_string(WTERMSIG(waitStatus)));
    } else {
        result.status_ = Status::unknown("process ended without an exit code");
    }

    return okResult(std::move(result));
}

} // namespace lc::process_runner_detail

#endif
