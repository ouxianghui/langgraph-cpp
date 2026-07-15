#include "foundation/logging/spd_logger.hpp"

#if LC_HAS_EXTERNAL_SPDLOG

#include "foundation/redaction/redactor.hpp"
#include "foundation/threading/thread_context.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace lgc {
namespace {

class ThreadNameFormatter final : public spdlog::custom_flag_formatter {
public:
    void format(
        const spdlog::details::log_msg& msg,
        const std::tm&,
        spdlog::memory_buf_t& dest) override
    {
        std::string name = ThreadContext::threadNameForLogThreadId(msg.thread_id);
        if (name.empty())
            name = "-";
        dest.append(name.data(), name.data() + name.size());
    }

    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override
    {
        return spdlog::details::make_unique<ThreadNameFormatter>();
    }
};

[[nodiscard]] std::unique_ptr<spdlog::formatter> createFormatter(const std::string& pattern)
{
    auto formatter = std::make_unique<spdlog::pattern_formatter>();
    formatter->add_flag<ThreadNameFormatter>('*').set_pattern(pattern);
    return formatter;
}

[[nodiscard]] spdlog::level::level_enum toSpdLevel(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warn:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    case LogLevel::Off:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

[[nodiscard]] spdlog::async_overflow_policy toSpdOverflowPolicy(SpdLogger::OverflowPolicy policy) noexcept
{
    switch (policy) {
    case SpdLogger::OverflowPolicy::Block:
        return spdlog::async_overflow_policy::block;
    case SpdLogger::OverflowPolicy::OverrunOldest:
        return spdlog::async_overflow_policy::overrun_oldest;
    }
    return spdlog::async_overflow_policy::block;
}

[[nodiscard]] Status validateAsyncOptions(const SpdLogger::AsyncOptions& options)
{
    if (options.queueSize_ == 0)
        return Status::invalidArgument("spd logger async queue size must be greater than zero");
    if (options.workerThreads_ == 0)
        return Status::invalidArgument("spd logger async worker count must be greater than zero");
    return Status::ok();
}

[[nodiscard]] bool sameAsyncOptions(
    const SpdLogger::AsyncOptions& lhs,
    const SpdLogger::AsyncOptions& rhs) noexcept
{
    return lhs.queueSize_ == rhs.queueSize_
        && lhs.workerThreads_ == rhs.workerThreads_
        && lhs.overflow_ == rhs.overflow_;
}

[[nodiscard]] Status ensureThreadPool(const SpdLogger::AsyncOptions& options)
{
    if (auto status = validateAsyncOptions(options); !status.isOk())
        return status;

    static std::mutex mutex;
    static bool configured = false;
    static SpdLogger::AsyncOptions configuredOptions;

    std::lock_guard lock(mutex);
    if (configured && spdlog::thread_pool()) {
        if (!sameAsyncOptions(configuredOptions, options))
            return Status::failedPrecondition("spd logger thread pool already initialized with different async options");
        return Status::ok();
    }

    try {
        if (!spdlog::thread_pool())
            spdlog::init_thread_pool(options.queueSize_, options.workerThreads_);
        configured = true;
        configuredOptions = options;
        return Status::ok();
    } catch (const std::exception& ex) {
        return Status::unavailable(std::string("spd logger thread pool initialization failed: ") + ex.what());
    } catch (...) {
        return Status::unknown("spd logger thread pool initialization failed");
    }
}

[[nodiscard]] Result<void> createParentDirectories(const std::string& path)
{
    const auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty())
        return okResult();

    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        return Status::unavailable(std::string("create log directory failed: ") + ec.message());
    }
    return okResult();
}

[[nodiscard]] Status validateLoggerText(
    std::string_view value,
    std::string_view label,
    std::size_t maxLength)
{
    if (value.empty()) {
        std::string message(label);
        message.append(" cannot be empty");
        return Status::invalidArgument(std::move(message));
    }
    if (maxLength != 0 && value.size() > maxLength) {
        std::string message(label);
        message.append(" is too long");
        return Status::invalidArgument(std::move(message));
    }
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) {
            std::string message(label);
            message.append(" contains a control character");
            return Status::invalidArgument(std::move(message));
        }
    }
    return Status::ok();
}

[[nodiscard]] Status validateCommonOptions(std::string_view name, std::string_view pattern)
{
    if (auto status = validateLoggerText(name, "spd logger name", 128); !status.isOk())
        return status;
    if (auto status = validateLoggerText(pattern, "spd logger pattern", 4096); !status.isOk())
        return status;
    return Status::ok();
}

[[nodiscard]] Status validateLogPath(const std::string& path)
{
    if (path.empty())
        return Status::invalidArgument("spd rotating file path cannot be empty");
    if (path.size() > 4096)
        return Status::invalidArgument("spd rotating file path is too long");
    const auto parsed = std::filesystem::path(path);
    if (parsed.filename().empty())
        return Status::invalidArgument("spd rotating file path must include a filename");
    return Status::ok();
}

[[nodiscard]] LogRecord redactRecord(LogRecord record, const Redactor& redactor)
{
    record.tag_ = redactor.redact(record.tag_);
    record.message_ = redactor.redact(record.message_);
    record.file_ = redactor.redact(record.file_);
    record.traceId_ = redactor.redact(record.traceId_);
    record.spanId_ = redactor.redact(record.spanId_);
    record.runId_ = redactor.redact(record.runId_);
    record.threadId_ = redactor.redact(record.threadId_);
    record.nodeId_ = redactor.redact(record.nodeId_);
    for (auto& [key, value] : record.fields_) {
        if (redactor.sensitiveKey(key))
            value = redactor.config().replacement_;
        else
            value = redactor.redact(value);
    }
    return record;
}

void addStandardFields(LogRecord::Fields& fields, const LogRecord& record)
{
    if (!record.traceId_.empty())
        fields.emplace("trace_id", record.traceId_);
    if (!record.spanId_.empty())
        fields.emplace("span_id", record.spanId_);
    if (!record.runId_.empty())
        fields.emplace("run_id", record.runId_);
    if (!record.threadId_.empty())
        fields.emplace("thread_id", record.threadId_);
    if (!record.nodeId_.empty())
        fields.emplace("node_id", record.nodeId_);
    if (record.statusCode_ != StatusCode::Ok)
        fields.emplace("status_code", std::string(statusCodeName(record.statusCode_)));
}

[[nodiscard]] std::string structuredMessage(const LogRecord& record)
{
    if (record.fields_.empty())
        return record.message_;

    std::ostringstream out;
    out << record.message_;
    for (const auto& [key, value] : record.fields_) {
        if (key.empty())
            continue;
        out << ' ' << key << "=\"";
        for (const char ch : value) {
            if (ch == '"' || ch == '\\')
                out << '\\';
            out << ch;
        }
        out << '"';
    }
    return out.str();
}

} // namespace

SpdLogger::SpdLogger(
    std::shared_ptr<spdlog::logger> logger,
    LogLevel minLevel,
    LogLimits limits,
    std::shared_ptr<const Redactor> redactor,
    bool redact) noexcept
    : logger_(std::move(logger))
    , minLevel_(minLevel)
    , limits_(std::move(limits))
    , redactor_(std::move(redactor))
    , redact_(redact)
{
    if (redact_ && !redactor_)
        redactor_ = std::make_shared<Redactor>();
    if (logger_)
        logger_->set_level(toSpdLevel(minLevel_));
}

Result<std::shared_ptr<SpdLogger>> SpdLogger::console(ConsoleOptions options)
{
    if (auto status = validateCommonOptions(options.name_, options.pattern_); !status.isOk())
        return status;
    if (auto status = ensureThreadPool(options.async_); !status.isOk())
        return status;

    try {
        auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto threadPool = spdlog::thread_pool();
        if (!threadPool)
            return Status::unavailable("spd logger thread pool is not available");

        auto logger = std::make_shared<spdlog::async_logger>(
            options.name_,
            std::move(sink),
            std::weak_ptr<spdlog::details::thread_pool>(threadPool),
            toSpdOverflowPolicy(options.async_.overflow_));
        logger->set_formatter(createFormatter(options.pattern_));
        logger->set_level(toSpdLevel(options.minLevel_));
        logger->flush_on(spdlog::level::warn);
        return std::make_shared<SpdLogger>(
            std::move(logger),
            options.minLevel_,
            options.limits_,
            options.redactor_,
            options.redact_);
    } catch (const std::exception& ex) {
        return Status::unavailable(std::string("create spd console logger failed: ") + ex.what());
    } catch (...) {
        return Status::unknown("create spd console logger failed");
    }
}

Result<std::shared_ptr<SpdLogger>> SpdLogger::rotatingFile(const RotatingFileOptions& options)
{
    if (auto status = validateCommonOptions(options.name_, options.pattern_); !status.isOk())
        return status;
    if (auto status = validateLogPath(options.path_); !status.isOk())
        return status;
    if (options.maxFileBytes_ == 0)
        return Status::invalidArgument("spd rotating file max bytes must be greater than zero");
    if (options.maxFiles_ == 0)
        return Status::invalidArgument("spd rotating file count must be greater than zero");
    if (auto status = ensureThreadPool(options.async_); !status.isOk())
        return status;
    if (auto result = createParentDirectories(options.path_); !result.isOk())
        return result.status();

    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            options.path_,
            options.maxFileBytes_,
            options.maxFiles_));

        auto threadPool = spdlog::thread_pool();
        if (!threadPool)
            return Status::unavailable("spd logger thread pool is not available");

        auto logger = std::make_shared<spdlog::async_logger>(
            options.name_,
            sinks.begin(),
            sinks.end(),
            std::weak_ptr<spdlog::details::thread_pool>(threadPool),
            toSpdOverflowPolicy(options.async_.overflow_));
        logger->set_formatter(createFormatter(options.pattern_));
        logger->set_level(toSpdLevel(options.minLevel_));
        logger->flush_on(spdlog::level::warn);
        return std::make_shared<SpdLogger>(
            std::move(logger),
            options.minLevel_,
            options.limits_,
            options.redactor_,
            options.redact_);
    } catch (const std::exception& ex) {
        return Status::unavailable(std::string("create spd rotating file logger failed: ") + ex.what());
    } catch (...) {
        return Status::unknown("create spd rotating file logger failed");
    }
}

void SpdLogger::log(const LogRecord& record) noexcept
{
    if (!logger_ || closed_.load() || record.level_ == LogLevel::Off)
        return;

    try {
        auto safe = record;
        addStandardFields(safe.fields_, safe);
        if (redact_ && redactor_)
            safe = redactRecord(std::move(safe), *redactor_);
        if (!validateLogRecord(safe, limits_).isOk())
            return;

        const auto level = toSpdLevel(safe.level_);
        if (!logger_->should_log(level))
            return;

        const auto source = spdlog::source_loc(
            safe.file_.empty() ? nullptr : safe.file_.c_str(),
            safe.line_,
            nullptr);
        const auto message = structuredMessage(safe);
        if (safe.tag_.empty())
            logger_->log(source, level, "{}", message);
        else
            logger_->log(source, level, "[{}] {}", safe.tag_, message);
    } catch (...) {
    }
}

Status SpdLogger::flush()
{
    try {
        if (closed_.load())
            return Status::ok();
        if (logger_)
            logger_->flush();
        return Status::ok();
    } catch (const std::exception& ex) {
        return Status::unavailable(std::string("spd logger flush failed: ") + ex.what());
    } catch (...) {
        return Status::unknown("spd logger flush failed");
    }
}

Status SpdLogger::close()
{
    if (closed_.exchange(true))
        return Status::ok();

    try {
        if (logger_)
            logger_->flush();
        return Status::ok();
    } catch (const std::exception& ex) {
        return Status::unavailable(std::string("spd logger close failed: ") + ex.what());
    } catch (...) {
        return Status::unknown("spd logger close failed");
    }
}

bool SpdLogger::isClosed() const noexcept
{
    return closed_.load();
}

void SpdLogger::setLevel(LogLevel value) noexcept
{
    minLevel_.store(value);
    try {
        if (logger_)
            logger_->set_level(toSpdLevel(value));
    } catch (...) {
    }
}

LogLevel SpdLogger::level() const noexcept
{
    return minLevel_.load();
}

std::shared_ptr<spdlog::logger> SpdLogger::handle() const noexcept
{
    return logger_;
}

} // namespace lgc

#endif
