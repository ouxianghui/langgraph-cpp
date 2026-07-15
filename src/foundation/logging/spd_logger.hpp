#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/status/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#ifndef LC_HAS_EXTERNAL_SPDLOG
#if __has_include(<spdlog/spdlog.h>)
#define LC_HAS_EXTERNAL_SPDLOG 1
#else
#define LC_HAS_EXTERNAL_SPDLOG 0
#endif
#endif

#if LC_HAS_EXTERNAL_SPDLOG
#include <spdlog/spdlog.h>
#endif

namespace lgc {

#if LC_HAS_EXTERNAL_SPDLOG

class SpdLogger final : public ILogger {
public:
    enum class OverflowPolicy : std::uint8_t {
        Block,
        OverrunOldest,
    };

    struct AsyncOptions {
        std::size_t queueSize_ { 8192 };
        std::size_t workerThreads_ { 1 };
        OverflowPolicy overflow_ { OverflowPolicy::Block };
    };

    struct ConsoleOptions {
        LogLevel minLevel_ { LogLevel::Info };
        std::string name_ { "langgraph_cpp" };
        std::string pattern_ { "[%Y-%m-%d %H:%M:%S.%e] [%t:%*] [%s:%#] [%n] [%^%l%$] %v" };
        AsyncOptions async_;
        bool redact_ { true };
        LogLimits limits_;
        std::shared_ptr<const Redactor> redactor_;
    };

    struct RotatingFileOptions {
        std::string path_;
        LogLevel minLevel_ { LogLevel::Info };
        std::string name_ { "langgraph_cpp" };
        std::string pattern_ { "[%Y-%m-%d %H:%M:%S.%e] [%t:%*] [%s:%#] [%n] [%^%l%$] %v" };
        std::size_t maxFileBytes_ { 10U * 1024U * 1024U };
        std::size_t maxFiles_ { 5U };
        AsyncOptions async_;
        bool redact_ { true };
        LogLimits limits_;
        std::shared_ptr<const Redactor> redactor_;
    };

    explicit SpdLogger(
        std::shared_ptr<spdlog::logger> logger,
        LogLevel minLevel = LogLevel::Info,
        LogLimits limits = {},
        std::shared_ptr<const Redactor> redactor = {},
        bool redact = true) noexcept;

    [[nodiscard]] static Result<std::shared_ptr<SpdLogger>> console(ConsoleOptions options = {});
    [[nodiscard]] static Result<std::shared_ptr<SpdLogger>> rotatingFile(const RotatingFileOptions& options);

    void log(const LogRecord& record) noexcept override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status close() override;
    [[nodiscard]] bool isClosed() const noexcept override;

    void setLevel(LogLevel value) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    [[nodiscard]] std::shared_ptr<spdlog::logger> handle() const noexcept;

private:
    std::shared_ptr<spdlog::logger> logger_;
    std::atomic<LogLevel> minLevel_ { LogLevel::Info };
    std::atomic_bool closed_ { false };
    LogLimits limits_;
    std::shared_ptr<const Redactor> redactor_;
    bool redact_ { true };
};

#endif

} // namespace lgc
