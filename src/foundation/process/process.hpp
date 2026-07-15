#pragma once

#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/status/result.hpp"
#include "foundation/time/clock.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lgc {

/// Called repeatedly to write stdin before the process starts. Return an empty string to finish.
using ProcessStdinProvider = std::function<Result<std::string>()>;

struct ProcessOptions {
    std::string executable_;
    std::vector<std::string> arguments_;
    bool shellAllowed_ { false };
    bool terminateProcessTree_ { true };
    bool inheritStdin_ { false };
    std::optional<std::filesystem::path> workingDirectory_;
    std::unordered_map<std::string, std::string> environment_;
    bool inheritEnvironment_ { true };
    std::optional<Clock::Duration> timeout_;
    CancellationToken cancellation_;
    std::optional<std::string> stdin_;
    ProcessStdinProvider stdinProvider_;
    std::size_t maxStdinBytes_ { 1024U * 1024U };
    std::size_t maxStdoutBytes_ { 1024U * 1024U };
    std::size_t maxStderrBytes_ { 1024U * 1024U };
};

struct ProcessResult {
    int exitCode_ { -1 };
    bool exited_ { false };
    bool timedOut_ { false };
    bool cancelled_ { false };
    bool stdoutTruncated_ { false };
    bool stderrTruncated_ { false };
    std::string stdout_;
    std::string stderr_;
    Status status_ { Status::unknown("process did not complete") };

    [[nodiscard]] bool success() const noexcept { return status_.isOk(); }
};

class ProcessRunner final {
public:
    explicit ProcessRunner(
        const Clock& clock = SteadyClock::instance(),
        std::shared_ptr<ILogger> logger = Logger::defaultLogger());

    [[nodiscard]] Result<ProcessResult> run(const ProcessOptions& options) const;

private:
    const Clock* clock_;
    std::shared_ptr<ILogger> logger_;
};

[[nodiscard]] Status validateProcessOptions(const ProcessOptions& options);
[[nodiscard]] Result<ProcessResult> runProcess(const ProcessOptions& options);

} // namespace lgc
