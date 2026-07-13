#include "foundation/process/process_runner_common.hh"

#include <utility>

namespace lc {

ProcessRunner::ProcessRunner(const Clock& clock, std::shared_ptr<ILogger> logger)
    : clock_(&clock)
    , logger_(std::move(logger))
{
}

Result<ProcessResult> ProcessRunner::run(const ProcessOptions& options) const
{
    if (auto status = validateProcessOptions(options); !status.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "ProcessRunner",
            "rejected process options executable={} status={}",
            __FILE__,
            __LINE__,
            options.executable_,
            status.toString());
        return errorResult<ProcessResult>(std::move(status));
    }
    if (auto status = options.cancellation_.check("process cancelled before start"); !status.isOk()) {
        logTo(logger_,
            LogLevel::Info,
            "ProcessRunner",
            "process cancelled before start executable={}",
            __FILE__,
            __LINE__,
            options.executable_);
        return errorResult<ProcessResult>(std::move(status));
    }

    logTo(logger_,
        LogLevel::Debug,
        "ProcessRunner",
        "starting process executable={} args={}",
        __FILE__,
        __LINE__,
        options.executable_,
        options.arguments_.size());

    auto result = process_runner_detail::runPlatformProcess(options, *clock_);
    if (!result.isOk()) {
        logTo(logger_,
            LogLevel::Warn,
            "ProcessRunner",
            "failed to start process executable={} status={}",
            __FILE__,
            __LINE__,
            options.executable_,
            result.status().toString());
        return result;
    }

    const auto level = result->status_.isOk() ? LogLevel::Debug : LogLevel::Warn;
    logTo(logger_,
        level,
        "ProcessRunner",
        "process finished executable={} exitCode={} status={}",
        __FILE__,
        __LINE__,
        options.executable_,
        result->exitCode_,
        result->status_.toString());
    return result;
}

Result<ProcessResult> runProcess(const ProcessOptions& options)
{
    return ProcessRunner().run(options);
}

} // namespace lc
