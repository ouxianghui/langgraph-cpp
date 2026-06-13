#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace lc {

/// Process-wide logging facade backed by spdlog.
///
/// Logging is asynchronous: producers enqueue records and sinks run on spdlog's global worker
/// pool. The first successful initialization wins; logging before explicit initialization lazily
/// creates a color stderr logger.
///
/// `Logger::shutdown()` drains and tears down spdlog's global async state. Avoid mixing this facade
/// with independent `spdlog::register_logger` users unless their lifetime is managed separately.
///
/// Compile-time verbosity: define `SPDLOG_ACTIVE_LEVEL` (see spdlog docs), e.g.
/// `-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO` for release builds.
class Logger {
public:
    using Level = spdlog::level::level_enum;

    /// Queue behavior when the async logger is saturated.
    enum class OverflowPolicy : std::uint8_t {
        Block,
        OverrunOldest,
        DiscardNew,
    };

    /// Async logger and worker-pool options.
    struct AsyncOptions {
        std::size_t queueSize = 8192;
        std::size_t workerThreads = 1;
        OverflowPolicy overflow = OverflowPolicy::Block;
    };

    /// Console logger options.
    struct ConsoleOptions {
        Level level = Level::info;
        std::string name = "bot_worker";
        AsyncOptions async;
    };

    /// Console + rotating file logger options.
    struct RotatingFileOptions {
        std::string path;
        Level level = Level::info;
        std::string name = "bot_worker";
        std::size_t maxFileBytes = 10U * 1024U * 1024U;
        std::size_t maxFiles = 5U;
        AsyncOptions async;
    };

    /// Legacy async options kept for existing call sites. Prefer `AsyncOptions` for new code.
    struct AsyncLogConfig {
        std::size_t queue_size = 8192;
        std::size_t worker_threads = 1;
        OverflowPolicy overflow = OverflowPolicy::Block;
    };

    /// Initialize a color stderr logger. Idempotent: the first successful configuration wins.
    static void initConsole();
    static void initConsole(const ConsoleOptions& options);

    /// Initialize a color stderr logger plus a rotating file sink.
    /// Creates parent directories best-effort. Idempotent: the first successful configuration wins.
    static void initRotatingFile(const RotatingFileOptions& options);

    /// Legacy console initializer. Prefer `initConsole`.
    static void initDefault(Level level = Level::info,
        std::string_view name = "bot_worker",
        const AsyncLogConfig* asyncConfig = nullptr);

    /// Legacy rotating-file initializer. Prefer `initRotatingFile`.
    static void initRotating(std::string_view path, Level level = Level::info,
        std::string_view name = "bot_worker",
        std::size_t maxFileBytes = 10U * 1024U * 1024U,
        std::size_t maxFiles = 5U,
        const AsyncLogConfig* asyncConfig = nullptr);

    /// Replace the active logger (e.g. tests with a null or memory sink). Thread-safe.
    static void reset(std::shared_ptr<spdlog::logger> logger);

    [[nodiscard]] static bool isConfigured() noexcept;

    /// Non-null after any init or first log line (lazy default).
    [[nodiscard]] static spdlog::logger* handle() noexcept;

    [[nodiscard]] static std::shared_ptr<spdlog::logger> loggerPtr() noexcept;

    static void setLevel(Level level) noexcept;
    [[nodiscard]] static Level getLevel() noexcept;

    static void setPattern(std::string_view pattern);
    static void flush() noexcept;

    /// Flushes, drops the active logger, and calls `spdlog::shutdown()` (drains global async pool).
    static void shutdown() noexcept;
};

} // namespace lc

// ----------------------------------------------------------------------------
// Level macros — delegate to spdlog so format strings are skipped when disabled.
//
// *(TAG, fmt, …): prepends one formatted segment "[TAG]" before your message (TAG can be a
// string literal, std::string, std::string_view, or any type fmt can print with "{}" ).
// Example: BW_LOG_INFO(WS, "session={}", id);  ->  … [WS] session=…
// ----------------------------------------------------------------------------

#define BW_LOG_TRACE(TAG, fmt, ...) \
    SPDLOG_LOGGER_TRACE(::lc::Logger::handle(), "[{}] " fmt, TAG __VA_OPT__(, ) __VA_ARGS__)
#define BW_LOG_DEBUG(TAG, fmt, ...) \
    SPDLOG_LOGGER_DEBUG(::lc::Logger::handle(), "[{}] " fmt, TAG __VA_OPT__(, ) __VA_ARGS__)
#define BW_LOG_INFO(TAG, fmt, ...) \
    SPDLOG_LOGGER_INFO(::lc::Logger::handle(), "[{}] " fmt, TAG __VA_OPT__(, ) __VA_ARGS__)
#define BW_LOG_WARN(TAG, fmt, ...) \
    SPDLOG_LOGGER_WARN(::lc::Logger::handle(), "[{}] " fmt, TAG __VA_OPT__(, ) __VA_ARGS__)
#define BW_LOG_ERROR(TAG, fmt, ...) \
    SPDLOG_LOGGER_ERROR(::lc::Logger::handle(), "[{}] " fmt, TAG __VA_OPT__(, ) __VA_ARGS__)
#define BW_LOG_CRITICAL(TAG, fmt, ...) \
    SPDLOG_LOGGER_CRITICAL(::lc::Logger::handle(), "[{}] " fmt, TAG __VA_OPT__(, ) __VA_ARGS__)
