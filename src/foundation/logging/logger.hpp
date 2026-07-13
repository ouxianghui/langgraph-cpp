#pragma once

#include "foundation/status/status.hpp"

#include <chrono>
#include <concepts>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lc {

class Redactor;

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

struct LogRecord {
    using Fields = std::map<std::string, std::string>;

    LogLevel level_ { LogLevel::Info };
    std::string tag_;
    std::string message_;
    std::chrono::system_clock::time_point timestamp_;
    std::string file_;
    int line_ { 0 };
    std::string traceId_;
    std::string spanId_;
    std::string runId_;
    std::string threadId_;
    std::string nodeId_;
    StatusCode statusCode_ { StatusCode::Ok };
    Fields fields_;
};

struct LogLimits {
    std::size_t maxTagLength_ { 128 };
    std::size_t maxMessageLength_ { 64 * 1024 };
    std::size_t maxSourceLength_ { 4096 };
    std::size_t maxFieldCount_ { 64 };
    std::size_t maxFieldKeyLength_ { 128 };
    std::size_t maxFieldValueLength_ { 16 * 1024 };
};

class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void log(const LogRecord& record) noexcept = 0;
    [[nodiscard]] virtual Status flush() = 0;
    [[nodiscard]] virtual Status close() = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

struct ConsoleLoggerOptions {
    LogLevel minLevel_ { LogLevel::Info };
    bool includeTimestamp_ { true };
    bool includeSource_ { true };
    bool redact_ { true };
    LogLimits limits_;
    std::shared_ptr<const Redactor> redactor_;
};

class ConsoleLogger final : public ILogger {
public:
    explicit ConsoleLogger(ConsoleLoggerOptions options = {});
    ConsoleLogger(std::ostream& stream, ConsoleLoggerOptions options = {});

    void log(const LogRecord& record) noexcept override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    std::ostream* stream_;
    ConsoleLoggerOptions options_;
    mutable std::mutex mutex_;
    bool closed_ { false };
};

class NullLogger final : public ILogger {
public:
    void log(const LogRecord& record) noexcept override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;
};

class Logger final {
public:
    [[nodiscard]] static std::shared_ptr<ILogger> defaultLogger();
    static void setDefaultLogger(std::shared_ptr<ILogger> logger) noexcept;
    static void useConsoleLogger(ConsoleLoggerOptions options = {});
    static void disable() noexcept;

    static void setLevel(LogLevel value) noexcept;
    [[nodiscard]] static LogLevel level() noexcept;
    [[nodiscard]] static bool shouldLog(LogLevel level) noexcept;

    static void log(
        LogLevel level,
        std::string_view tag,
        std::string message,
        const char* file = nullptr,
        int line = 0) noexcept;

    static void log(LogRecord record) noexcept;

    [[nodiscard]] static Status flush();
    [[nodiscard]] static Status close();
    [[nodiscard]] static bool isClosed() noexcept;

private:
    Logger() = delete;
};

[[nodiscard]] std::string_view logLevelName(LogLevel level) noexcept;
[[nodiscard]] Status validateLogRecord(const LogRecord& record, const LogLimits& limits = {});

void logTo(
    const std::shared_ptr<ILogger>& logger,
    LogLevel level,
    std::string_view tag,
    std::string message,
    const char* file = nullptr,
    int line = 0) noexcept;

void logTo(const std::shared_ptr<ILogger>& logger, LogRecord record) noexcept;

namespace detail {

template <typename T>
[[nodiscard]] std::string logValueToString(T&& value)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::same_as<Value, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::same_as<Value, std::string>) {
        return std::forward<T>(value);
    } else if constexpr (std::same_as<Value, std::string_view>) {
        return std::string(value);
    } else if constexpr (std::is_pointer_v<Value>
        && std::same_as<std::remove_cv_t<std::remove_pointer_t<Value>>, char>) {
        return value == nullptr ? std::string("(null)") : std::string(value);
    } else if constexpr (requires(std::ostringstream& out, T&& item) {
                             out << std::forward<T>(item);
                         }) {
        std::ostringstream out;
        out << std::forward<T>(value);
        return out.str();
    } else {
        return "<?>";
    }
}

inline bool replaceNextPlaceholder(std::string& message, std::size_t& searchOffset, std::string_view value)
{
    const auto placeholder = message.find("{}", searchOffset);
    if (placeholder == std::string::npos)
        return false;

    message.replace(placeholder, 2, value.data(), value.size());
    searchOffset = placeholder + value.size();
    return true;
}

inline void appendExtraLogValue(std::string& message, std::string_view value)
{
    if (!message.empty())
        message.push_back(' ');
    message.append(value.data(), value.size());
}

} // namespace detail

template <typename... Args>
[[nodiscard]] std::string formatLogMessage(std::string_view format, Args&&... args)
{
    std::string message(format);
    std::size_t searchOffset = 0;
    auto applyOne = [&](auto&& value) {
        const auto text = detail::logValueToString(std::forward<decltype(value)>(value));
        if (!detail::replaceNextPlaceholder(message, searchOffset, text))
            detail::appendExtraLogValue(message, text);
    };
    (applyOne(std::forward<Args>(args)), ...);
    return message;
}

template <typename... Args>
void logTo(
    const std::shared_ptr<ILogger>& logger,
    LogLevel level,
    std::string_view tag,
    std::string_view format,
    const char* file,
    int line,
    Args&&... args) noexcept
{
    if (!logger || level == LogLevel::Off)
        return;
    logTo(
        logger,
        level,
        tag,
        formatLogMessage(format, std::forward<Args>(args)...),
        file,
        line);
}

} // namespace lc

#define LOG_TRACE(TAG, fmt, ...)                                                \
    do {                                                                        \
        if (::lc::Logger::shouldLog(::lc::LogLevel::Trace))                     \
            ::lc::Logger::log(::lc::LogLevel::Trace, TAG,                       \
                ::lc::formatLogMessage(fmt __VA_OPT__(, ) __VA_ARGS__),         \
                __FILE__, __LINE__);                                            \
    } while (false)

#define LOG_DEBUG(TAG, fmt, ...)                                                \
    do {                                                                        \
        if (::lc::Logger::shouldLog(::lc::LogLevel::Debug))                     \
            ::lc::Logger::log(::lc::LogLevel::Debug, TAG,                       \
                ::lc::formatLogMessage(fmt __VA_OPT__(, ) __VA_ARGS__),         \
                __FILE__, __LINE__);                                            \
    } while (false)

#define LOG_INFO(TAG, fmt, ...)                                                 \
    do {                                                                        \
        if (::lc::Logger::shouldLog(::lc::LogLevel::Info))                      \
            ::lc::Logger::log(::lc::LogLevel::Info, TAG,                        \
                ::lc::formatLogMessage(fmt __VA_OPT__(, ) __VA_ARGS__),         \
                __FILE__, __LINE__);                                            \
    } while (false)

#define LOG_WARN(TAG, fmt, ...)                                                 \
    do {                                                                        \
        if (::lc::Logger::shouldLog(::lc::LogLevel::Warn))                      \
            ::lc::Logger::log(::lc::LogLevel::Warn, TAG,                        \
                ::lc::formatLogMessage(fmt __VA_OPT__(, ) __VA_ARGS__),         \
                __FILE__, __LINE__);                                            \
    } while (false)

#define LOG_ERROR(TAG, fmt, ...)                                                \
    do {                                                                        \
        if (::lc::Logger::shouldLog(::lc::LogLevel::Error))                     \
            ::lc::Logger::log(::lc::LogLevel::Error, TAG,                       \
                ::lc::formatLogMessage(fmt __VA_OPT__(, ) __VA_ARGS__),         \
                __FILE__, __LINE__);                                            \
    } while (false)

#define LOG_CRITICAL(TAG, fmt, ...)                                             \
    do {                                                                        \
        if (::lc::Logger::shouldLog(::lc::LogLevel::Critical))                  \
            ::lc::Logger::log(::lc::LogLevel::Critical, TAG,                    \
                ::lc::formatLogMessage(fmt __VA_OPT__(, ) __VA_ARGS__),         \
                __FILE__, __LINE__);                                            \
    } while (false)
