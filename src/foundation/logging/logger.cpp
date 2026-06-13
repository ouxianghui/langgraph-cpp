#include "foundation/logging/logger.hpp"

#include "foundation/threading/thread_context.hpp"

#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace fs = std::filesystem;

namespace lc {
namespace {

std::mutex& loggerMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<spdlog::logger>& activeLogger()
{
    static std::shared_ptr<spdlog::logger> logger;
    return logger;
}

Logger::AsyncOptions defaultAsyncOptions()
{
    return Logger::AsyncOptions {};
}

Logger::AsyncOptions toAsyncOptions(const Logger::AsyncLogConfig& config)
{
    return Logger::AsyncOptions {
        .queueSize = config.queue_size,
        .workerThreads = config.worker_threads,
        .overflow = config.overflow,
    };
}

class ThreadNameFormatter final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg,
        const std::tm&,
        spdlog::memory_buf_t& dest) override
    {
        std::string name = ThreadContext::threadNameForLogThreadId(msg.thread_id);
        if (name.empty()) {
            name = "-";
        }
        dest.append(name.data(), name.data() + name.size());
    }

    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override
    {
        return spdlog::details::make_unique<ThreadNameFormatter>();
    }
};

std::unique_ptr<spdlog::formatter> createFormatter(std::string_view pattern)
{
    auto formatter = std::make_unique<spdlog::pattern_formatter>();
    formatter->add_flag<ThreadNameFormatter>('*').set_pattern(std::string(pattern));
    return formatter;
}

void installAsyncPool(const Logger::AsyncOptions& options)
{
    spdlog::init_thread_pool(options.queueSize, options.workerThreads);
}

[[nodiscard]] spdlog::async_overflow_policy toSpdlogOverflowPolicy(Logger::OverflowPolicy policy)
{
    switch (policy) {
    case Logger::OverflowPolicy::Block:
        return spdlog::async_overflow_policy::block;
    case Logger::OverflowPolicy::OverrunOldest:
        return spdlog::async_overflow_policy::overrun_oldest;
    case Logger::OverflowPolicy::DiscardNew:
        return spdlog::async_overflow_policy::discard_new;
    }
    return spdlog::async_overflow_policy::block;
}

void configureLogger(const std::shared_ptr<spdlog::logger>& logger,
    spdlog::level::level_enum level)
{
    logger->set_level(level);
    // %Y-%m-%d ... : wall-clock datetime
    // %t             : thread id
    // %*             : lc thread name (custom flag, resolved from log record thread id)
    // %s:%#          : short source file name + line (requires SPDLOG_LOGGER_* / BW_LOG_* source_loc)
    // %n             : logger name (tag)
    // %^%l%$         : level with color range (TTY sinks)
    // %v             : message payload
    logger->set_formatter(createFormatter("[%Y-%m-%d %H:%M:%S.%e] [%t:%*] [%s:%#] [%n] [%^%l%$] %v"));
    logger->flush_on(spdlog::level::warn);
}

std::shared_ptr<spdlog::logger> createConsoleLogger(std::string_view name,
    spdlog::level::level_enum level,
    spdlog::async_overflow_policy overflow)
{
    auto threadPool = spdlog::thread_pool();
    auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks = { stderrSink };
    auto logger = std::make_shared<spdlog::async_logger>(
        std::string(name), sinks.begin(), sinks.end(), std::weak_ptr<spdlog::details::thread_pool>(threadPool),
        overflow);
    spdlog::register_logger(logger);
    configureLogger(logger, level);
    return logger;
}

std::shared_ptr<spdlog::logger> createRotatingLogger(std::string_view logFilePath,
    spdlog::level::level_enum level,
    std::string_view loggerName,
    std::size_t maxFileBytes,
    std::size_t maxRotatedFiles,
    spdlog::async_overflow_policy overflow)
{
    auto threadPool = spdlog::thread_pool();
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        std::string(logFilePath), maxFileBytes, maxRotatedFiles));

    auto logger = std::make_shared<spdlog::async_logger>(
        std::string(loggerName), sinks.begin(), sinks.end(),
        std::weak_ptr<spdlog::details::thread_pool>(threadPool), overflow);
    spdlog::register_logger(logger);
    configureLogger(logger, level);
    return logger;
}

void ensureAsyncPool()
{
    if (!spdlog::thread_pool()) {
        installAsyncPool(defaultAsyncOptions());
    }
}

void createParentDirectories(std::string_view path)
{
    fs::path logPath(path);
    if (!logPath.has_parent_path()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(logPath.parent_path(), ec);
}

} // namespace

bool Logger::isConfigured() noexcept
{
    return static_cast<bool>(activeLogger());
}

std::shared_ptr<spdlog::logger> Logger::loggerPtr() noexcept
{
    auto& logger = activeLogger();
    if (logger) {
        return logger;
    }
    std::lock_guard<std::mutex> lock(loggerMutex());
    if (!logger) {
        ensureAsyncPool();
        logger = createConsoleLogger("bot_worker", Level::info, spdlog::async_overflow_policy::block);
    }
    return logger;
}

spdlog::logger* Logger::handle() noexcept
{
    return loggerPtr().get();
}

void Logger::initDefault(Level level, std::string_view name,
    const AsyncLogConfig* asyncConfig)
{
    ConsoleOptions options;
    options.level = level;
    options.name = name;
    if (asyncConfig) {
        options.async = toAsyncOptions(*asyncConfig);
    }
    initConsole(options);
}

void Logger::initConsole()
{
    initConsole(ConsoleOptions {});
}

void Logger::initConsole(const ConsoleOptions& options)
{
    std::lock_guard<std::mutex> lock(loggerMutex());
    auto& logger = activeLogger();
    if (logger) {
        return;
    }
    installAsyncPool(options.async);
    logger = createConsoleLogger(options.name, options.level, toSpdlogOverflowPolicy(options.async.overflow));
}

void Logger::initRotating(std::string_view path, Level level,
    std::string_view name, std::size_t maxFileBytes,
    std::size_t maxFiles, const AsyncLogConfig* asyncConfig)
{
    RotatingFileOptions options;
    options.path = path;
    options.level = level;
    options.name = name;
    options.maxFileBytes = maxFileBytes;
    options.maxFiles = maxFiles;
    if (asyncConfig) {
        options.async = toAsyncOptions(*asyncConfig);
    }
    initRotatingFile(options);
}

void Logger::initRotatingFile(const RotatingFileOptions& options)
{
    std::lock_guard<std::mutex> lock(loggerMutex());
    auto& logger = activeLogger();
    if (logger) {
        return;
    }

    createParentDirectories(options.path);

    installAsyncPool(options.async);
    logger = createRotatingLogger(options.path, options.level, options.name, options.maxFileBytes, options.maxFiles,
        toSpdlogOverflowPolicy(options.async.overflow));
}

void Logger::reset(std::shared_ptr<spdlog::logger> logger)
{
    std::lock_guard<std::mutex> lock(loggerMutex());
    auto& current = activeLogger();
    if (current) {
        spdlog::drop(current->name());
        current.reset();
    }
    current = std::move(logger);
    if (current) {
        spdlog::register_logger(current);
    }
}

void Logger::setLevel(Level level) noexcept
{
    loggerPtr()->set_level(level);
}

Logger::Level Logger::getLevel() noexcept
{
    return loggerPtr()->level();
}

void Logger::setPattern(std::string_view pattern)
{
    loggerPtr()->set_formatter(createFormatter(pattern));
}

void Logger::flush() noexcept
{
    try {
        loggerPtr()->flush();
    } catch (...) {
        // Logging backends must not throw through noexcept boundaries used by sinks / async flush.
    }
}

void Logger::shutdown() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(loggerMutex());
        auto& logger = activeLogger();
        if (logger) {
            try {
                logger->flush();
            } catch (...) {
            }
            spdlog::drop(logger->name());
            logger.reset();
        }
        spdlog::shutdown();
    } catch (...) {
    }
}

} // namespace lc
