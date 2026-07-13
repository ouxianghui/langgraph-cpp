#include "foundation/logging/logger.hpp"

#include "foundation/redaction/redactor.hpp"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace lc {
namespace {

std::mutex& registryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<ILogger>& defaultLoggerStorage()
{
    static std::shared_ptr<ILogger> logger;
    return logger;
}

LogLevel& minimumLevelStorage()
{
    static LogLevel level = LogLevel::Info;
    return level;
}

[[nodiscard]] bool isEnabled(LogLevel recordLevel, LogLevel minimumLevel) noexcept
{
    if (minimumLevel == LogLevel::Off || recordLevel == LogLevel::Off)
        return false;
    return static_cast<int>(recordLevel) >= static_cast<int>(minimumLevel);
}

[[nodiscard]] std::string timestampString(std::chrono::system_clock::time_point value)
{
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm tm {};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

[[nodiscard]] std::string fieldsString(const LogRecord::Fields& fields)
{
    std::string out;
    for (const auto& [key, value] : fields) {
        if (key.empty())
            continue;
        if (!out.empty())
            out.push_back(' ');
        out.append(key);
        out.push_back('=');
        out.push_back('"');
        for (const char ch : value) {
            if (ch == '"' || ch == '\\')
                out.push_back('\\');
            out.push_back(ch);
        }
        out.push_back('"');
    }
    return out;
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

[[nodiscard]] Status validateLogText(
    std::string_view value,
    std::string_view label,
    std::size_t maxLength,
    bool allowNewline)
{
    if (maxLength != 0 && value.size() > maxLength) {
        std::string message(label);
        message.append(" is too long");
        return Status::invalidArgument(std::move(message));
    }

    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte == 0x7f || (byte < 0x20 && ch != '\t' && (ch != '\n' || !allowNewline))) {
            std::string message(label);
            message.append(" contains a control character");
            return Status::invalidArgument(std::move(message));
        }
    }
    return Status::ok();
}

[[nodiscard]] LogRecord redactRecord(LogRecord record, const ConsoleLoggerOptions& options)
{
    if (!options.redact_ || !options.redactor_)
        return record;

    record.tag_ = options.redactor_->redact(record.tag_);
    record.message_ = options.redactor_->redact(record.message_);
    record.file_ = options.redactor_->redact(record.file_);
    record.traceId_ = options.redactor_->redact(record.traceId_);
    record.spanId_ = options.redactor_->redact(record.spanId_);
    record.runId_ = options.redactor_->redact(record.runId_);
    record.threadId_ = options.redactor_->redact(record.threadId_);
    record.nodeId_ = options.redactor_->redact(record.nodeId_);
    for (auto& [key, value] : record.fields_) {
        if (options.redactor_->sensitiveKey(key))
            value = options.redactor_->config().replacement_;
        else
            value = options.redactor_->redact(value);
    }
    return record;
}

void normalizeRecord(LogRecord& record)
{
    if (record.timestamp_ == std::chrono::system_clock::time_point {})
        record.timestamp_ = std::chrono::system_clock::now();
    addStandardFields(record.fields_, record);
}

} // namespace

ConsoleLogger::ConsoleLogger(ConsoleLoggerOptions options)
    : ConsoleLogger(std::clog, options)
{
}

ConsoleLogger::ConsoleLogger(std::ostream& stream, ConsoleLoggerOptions options)
    : stream_(&stream)
    , options_(std::move(options))
{
    if (options_.redact_ && !options_.redactor_)
        options_.redactor_ = std::make_shared<Redactor>();
}

void ConsoleLogger::log(const LogRecord& record) noexcept
{
    try {
        std::lock_guard lock(mutex_);
        if (closed_ || !isEnabled(record.level_, options_.minLevel_))
            return;

        auto safe = record;
        normalizeRecord(safe);
        safe = redactRecord(std::move(safe), options_);
        if (!validateLogRecord(safe, options_.limits_).isOk())
            return;

        if (options_.includeTimestamp_)
            *stream_ << timestampString(safe.timestamp_) << ' ';
        *stream_ << '[' << logLevelName(safe.level_) << "] ";
        if (!safe.tag_.empty())
            *stream_ << '[' << safe.tag_ << "] ";
        *stream_ << safe.message_;
        const auto fields = fieldsString(safe.fields_);
        if (!fields.empty())
            *stream_ << ' ' << fields;
        if (options_.includeSource_ && !safe.file_.empty())
            *stream_ << " (" << safe.file_ << ':' << safe.line_ << ')';
        *stream_ << '\n';
    } catch (...) {
    }
}

Status ConsoleLogger::flush()
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::ok();

    try {
        stream_->flush();
        if (!(*stream_))
            return Status::unavailable("console logger flush failed");
        return Status::ok();
    } catch (...) {
        return Status::unknown("console logger flush failed");
    }
}

Status ConsoleLogger::close()
{
    std::lock_guard lock(mutex_);
    if (closed_)
        return Status::ok();

    try {
        stream_->flush();
        closed_ = true;
        if (!(*stream_))
            return Status::unavailable("console logger close flush failed");
        return Status::ok();
    } catch (...) {
        closed_ = true;
        return Status::unknown("console logger close failed");
    }
}

bool ConsoleLogger::isClosed() const noexcept
{
    try {
        std::lock_guard lock(mutex_);
        return closed_;
    } catch (...) {
        return true;
    }
}

void NullLogger::log(const LogRecord&) noexcept
{
}

Status NullLogger::flush()
{
    return Status::ok();
}

Status NullLogger::close()
{
    return Status::ok();
}

bool NullLogger::isClosed() const noexcept
{
    return true;
}

std::shared_ptr<ILogger> Logger::defaultLogger()
{
    std::lock_guard lock(registryMutex());
    auto& logger = defaultLoggerStorage();
    if (!logger)
        logger = std::make_shared<ConsoleLogger>();
    return logger;
}

void Logger::setDefaultLogger(std::shared_ptr<ILogger> logger) noexcept
{
    std::lock_guard lock(registryMutex());
    defaultLoggerStorage() = logger ? std::move(logger) : std::make_shared<NullLogger>();
}

void Logger::useConsoleLogger(ConsoleLoggerOptions options)
{
    setDefaultLogger(std::make_shared<ConsoleLogger>(std::move(options)));
}

void Logger::disable() noexcept
{
    std::lock_guard lock(registryMutex());
    minimumLevelStorage() = LogLevel::Off;
    defaultLoggerStorage() = std::make_shared<NullLogger>();
}

void Logger::setLevel(LogLevel value) noexcept
{
    std::lock_guard lock(registryMutex());
    minimumLevelStorage() = value;
}

LogLevel Logger::level() noexcept
{
    std::lock_guard lock(registryMutex());
    return minimumLevelStorage();
}

bool Logger::shouldLog(LogLevel level) noexcept
{
    std::lock_guard lock(registryMutex());
    return isEnabled(level, minimumLevelStorage());
}

void Logger::log(
    LogLevel level,
    std::string_view tag,
    std::string message,
    const char* file,
    int line) noexcept
{
    log(LogRecord {
        .level_ = level,
        .tag_ = std::string(tag),
        .message_ = std::move(message),
        .timestamp_ = std::chrono::system_clock::now(),
        .file_ = file == nullptr ? std::string() : std::string(file),
        .line_ = line,
    });
}

void Logger::log(LogRecord record) noexcept
{
    std::shared_ptr<ILogger> logger;
    {
        std::lock_guard lock(registryMutex());
        if (!isEnabled(record.level_, minimumLevelStorage()))
            return;
        auto& stored = defaultLoggerStorage();
        if (!stored)
            stored = std::make_shared<ConsoleLogger>();
        logger = stored;
    }

    normalizeRecord(record);
    if (!validateLogRecord(record).isOk())
        return;
    logger->log(record);
}

Status Logger::flush()
{
    auto logger = defaultLogger();
    try {
        return logger->flush();
    } catch (...) {
        return Status::unknown("default logger flush failed");
    }
}

Status Logger::close()
{
    auto logger = defaultLogger();
    try {
        return logger->close();
    } catch (...) {
        return Status::unknown("default logger close failed");
    }
}

bool Logger::isClosed() noexcept
{
    auto logger = defaultLogger();
    return logger->isClosed();
}

void logTo(
    const std::shared_ptr<ILogger>& logger,
    LogLevel level,
    std::string_view tag,
    std::string message,
    const char* file,
    int line) noexcept
{
    if (!logger || level == LogLevel::Off)
        return;

    logTo(logger, LogRecord {
        .level_ = level,
        .tag_ = std::string(tag),
        .message_ = std::move(message),
        .timestamp_ = std::chrono::system_clock::now(),
        .file_ = file == nullptr ? std::string() : std::string(file),
        .line_ = line,
    });
}

void logTo(const std::shared_ptr<ILogger>& logger, LogRecord record) noexcept
{
    if (!logger || record.level_ == LogLevel::Off)
        return;

    normalizeRecord(record);
    if (!validateLogRecord(record).isOk())
        return;
    logger->log(record);
}

std::string_view logLevelName(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    case LogLevel::Off:
        return "off";
    }
    return "unknown";
}

Status validateLogRecord(const LogRecord& record, const LogLimits& limits)
{
    if (record.level_ == LogLevel::Off)
        return Status::ok();
    if (record.line_ < 0)
        return Status::invalidArgument("log line cannot be negative");
    if (auto status = validateLogText(record.tag_, "log tag", limits.maxTagLength_, false); !status.isOk())
        return status;
    if (auto status = validateLogText(record.message_, "log message", limits.maxMessageLength_, true); !status.isOk())
        return status;
    if (auto status = validateLogText(record.file_, "log source", limits.maxSourceLength_, false); !status.isOk())
        return status;
    if (limits.maxFieldCount_ != 0 && record.fields_.size() > limits.maxFieldCount_)
        return Status::invalidArgument("log record has too many fields");
    for (const auto& [key, value] : record.fields_) {
        if (key.empty())
            return Status::invalidArgument("log field key cannot be empty");
        if (auto status = validateLogText(key, "log field key", limits.maxFieldKeyLength_, false); !status.isOk())
            return status;
        if (auto status = validateLogText(value, "log field value", limits.maxFieldValueLength_, true); !status.isOk())
            return status;
    }
    return Status::ok();
}

} // namespace lc
