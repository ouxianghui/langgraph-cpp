#include "foundation/executor/inline_executor.hpp"
#include "foundation/logging/logger.hpp"
#include "foundation/logging/spd_logger.hpp"
#include "foundation/process/process.hpp"
#include "foundation/rate_limit/circuit_breaker.hpp"
#include "foundation/scheduler/scheduler.hpp"
#include "foundation/threading/thread_pool.hpp"
#include "foundation/timer/interval_timer.hpp"

#include <cassert>
#include <chrono>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if LC_HAS_EXTERNAL_SPDLOG
#include <spdlog/sinks/ostream_sink.h>
#endif

namespace {

class MemoryLogger final : public lc::ILogger {
public:
    void log(const lc::LogRecord& record) noexcept override
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return;
        records_.push_back(record);
    }

    lc::Status flush() override
    {
        std::lock_guard lock(mutex_);
        flushed_ = true;
        return lc::Status::ok();
    }

    lc::Status close() override
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        return lc::Status::ok();
    }

    bool isClosed() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    std::size_t size() const
    {
        std::lock_guard lock(mutex_);
        return records_.size();
    }

    bool containsTag(std::string_view tag) const
    {
        std::lock_guard lock(mutex_);
        for (const auto& record : records_) {
            if (record.tag_ == tag)
                return true;
        }
        return false;
    }

    mutable std::mutex mutex_;
    std::vector<lc::LogRecord> records_;
    bool flushed_ { false };
    bool closed_ { false };
};

class FailingLogger final : public lc::ILogger {
public:
    void log(const lc::LogRecord&) noexcept override {}
    lc::Status flush() override { return lc::Status::unavailable("flush failed"); }
    lc::Status close() override { return lc::Status::aborted("close failed"); }
    bool isClosed() const noexcept override { return false; }
};

} // namespace

int main()
{
    auto logger = std::make_shared<MemoryLogger>();
    lc::Logger::setDefaultLogger(logger);
    lc::Logger::setLevel(lc::LogLevel::Debug);

    assert(lc::Logger::shouldLog(lc::LogLevel::Info));
    assert(!lc::Logger::shouldLog(lc::LogLevel::Trace));
    assert(lc::formatLogMessage("value={}", 42) == "value=42");
    assert(lc::formatLogMessage("plain", 42, true) == "plain 42 true");

    lc::Logger::log(lc::LogLevel::Info, "Test", "hello", "file.cpp", 7);
    LOG_WARN("Macro", "value={}", 42);
    lc::Logger::log(lc::LogRecord {
        .level_ = lc::LogLevel::Info,
        .tag_ = "Structured",
        .message_ = "state",
        .traceId_ = "trace-1",
        .spanId_ = "span-1",
        .statusCode_ = lc::StatusCode::Unavailable,
        .fields_ = {
            { "run_id", "run-1" },
            { "node_id", "node-a" },
        },
    });

    assert(logger->size() == 3);
    {
        std::lock_guard lock(logger->mutex_);
        assert(logger->records_[0].level_ == lc::LogLevel::Info);
        assert(logger->records_[0].tag_ == "Test");
        assert(logger->records_[0].message_ == "hello");
        assert(logger->records_[1].level_ == lc::LogLevel::Warn);
        assert(logger->records_[1].tag_ == "Macro");
        assert(logger->records_[1].message_ == "value=42");
        assert(logger->records_[2].fields_.at("run_id") == "run-1");
        assert(logger->records_[2].fields_.at("trace_id") == "trace-1");
        assert(logger->records_[2].fields_.at("span_id") == "span-1");
        assert(logger->records_[2].fields_.at("status_code") == "unavailable");
    }

    assert(lc::Logger::flush().isOk());
    assert(logger->flushed_);

    assert(lc::Logger::close().isOk());
    assert(lc::Logger::isClosed());
    LOG_WARN("Closed", "hidden");
    assert(logger->size() == 3);

    auto failing = std::make_shared<FailingLogger>();
    lc::Logger::setDefaultLogger(failing);
    lc::Logger::setLevel(lc::LogLevel::Info);
    assert(lc::Logger::flush().code() == lc::StatusCode::Unavailable);
    assert(lc::Logger::close().code() == lc::StatusCode::Aborted);

    auto concurrent = std::make_shared<MemoryLogger>();
    lc::Logger::setDefaultLogger(concurrent);
    lc::Logger::setLevel(lc::LogLevel::Info);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread) {
        threads.emplace_back([thread] {
            for (int i = 0; i < 100; ++i)
                lc::Logger::log(lc::LogLevel::Info, "Concurrent", lc::formatLogMessage("t={} i={}", thread, i));
        });
    }
    for (auto& thread : threads)
        thread.join();
    assert(concurrent->size() == 800);

    std::ostringstream consoleOut;
    lc::ConsoleLogger console(consoleOut, lc::ConsoleLoggerOptions {
        .minLevel_ = lc::LogLevel::Info,
        .includeTimestamp_ = false,
        .includeSource_ = false,
    });
    console.log(lc::LogRecord {
        .level_ = lc::LogLevel::Info,
        .tag_ = "Console",
        .message_ = "payload sk-1234567890abcdef",
        .traceId_ = "trace-console",
        .statusCode_ = lc::StatusCode::Unauthenticated,
        .fields_ = {
            { "api_key", "sk-1234567890abcdef" },
            { "run_id", "run-2" },
        },
    });
    assert(console.flush().isOk());
    assert(consoleOut.str().find("run_id=\"run-2\"") != std::string::npos);
    assert(consoleOut.str().find("trace_id=\"trace-console\"") != std::string::npos);
    assert(consoleOut.str().find("status_code=\"unauthenticated\"") != std::string::npos);
    assert(consoleOut.str().find("sk-1234567890abcdef") == std::string::npos);
    assert(consoleOut.str().find("api_key=\"[REDACTED]\"") != std::string::npos);
    assert(console.close().isOk());
    assert(console.isClosed());

    std::ostringstream limitedOut;
    lc::ConsoleLogger limitedConsole(limitedOut, lc::ConsoleLoggerOptions {
        .includeTimestamp_ = false,
        .includeSource_ = false,
        .limits_ = lc::LogLimits {
            .maxMessageLength_ = 4,
        },
    });
    limitedConsole.log(lc::LogRecord {
        .level_ = lc::LogLevel::Info,
        .message_ = "too long",
    });
    assert(limitedOut.str().empty());

    lc::Logger::disable();
    assert(!lc::Logger::shouldLog(lc::LogLevel::Critical));
    LOG_ERROR("Macro", "hidden");
    assert(logger->size() == 3);

    lc::Logger::useConsoleLogger(lc::ConsoleLoggerOptions {
        .minLevel_ = lc::LogLevel::Off,
        .includeTimestamp_ = false,
        .includeSource_ = false,
    });
    lc::Logger::setLevel(lc::LogLevel::Info);
    assert(lc::logLevelName(lc::LogLevel::Critical) == "critical");

    auto injected = std::make_shared<MemoryLogger>();
    lc::InlineExecutor inlineExecutor(injected);
    auto taskStatus = inlineExecutor.execute([] {
        throw std::runtime_error("boom");
    });
    assert(!taskStatus.isOk());
    assert(injected->containsTag("InlineExecutor"));

    lc::IntervalTimer timer(injected);
    timer.setSingleShot(true);
    timer.setHandler([] {
        throw std::runtime_error("timer boom");
    });
    assert(timer.start(std::chrono::milliseconds(1)).isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(injected->containsTag("IntervalTimer"));

    lc::ThreadPool pool(1, 0, injected);
    assert(pool.submit([] {
        throw std::runtime_error("pool boom");
    }).isOk());
    assert(pool.waitIdle(std::chrono::seconds(1)).isOk());
    assert(pool.shutdown(std::chrono::seconds(1)).isOk());
    assert(injected->containsTag("ThreadPool"));

    lc::ProcessRunner runner(lc::SteadyClock::instance(), injected);
    auto result = runner.run(lc::ProcessOptions {});
    assert(!result.isOk());
    assert(injected->containsTag("ProcessRunner"));

    lc::CircuitBreaker breaker(lc::CircuitBreakerPolicy { .failureThreshold_ = 1 }, lc::SteadyClock::instance(), injected);
    breaker.recordFailure();
    assert(injected->containsTag("CircuitBreaker"));

    lc::TaskScheduler scheduler(lc::SchedulerOptions {
        .logger_ = injected,
    });
    assert(scheduler.scheduleAfter(std::chrono::milliseconds(1), [] {
        throw std::runtime_error("scheduled boom");
    }).isOk());
    assert(scheduler.waitIdle(std::chrono::seconds(1)).isOk());
    assert(scheduler.close(std::chrono::seconds(1)).isOk());
    assert(injected->containsTag("TaskScheduler"));

#if LC_HAS_EXTERNAL_SPDLOG
    std::ostringstream output;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    auto spdlogLogger = std::make_shared<spdlog::logger>("langgraph_cpp_logging_test", std::move(sink));
    auto spdLogger = std::make_shared<lc::SpdLogger>(std::move(spdlogLogger), lc::LogLevel::Debug);

    lc::Logger::setDefaultLogger(spdLogger);
    lc::Logger::setLevel(lc::LogLevel::Debug);
    LOG_INFO("Spd", "backend sk-1234567890abcdef");
    assert(lc::Logger::flush().isOk());
    assert(output.str().find("[Spd] backend") != std::string::npos);
    assert(output.str().find("sk-1234567890abcdef") == std::string::npos);
    assert(lc::Logger::close().isOk());
    assert(spdLogger->isClosed());

    auto invalidConsole = lc::SpdLogger::console(lc::SpdLogger::ConsoleOptions {
        .name_ = "",
    });
    assert(!invalidConsole.isOk());
    assert(invalidConsole.status().code() == lc::StatusCode::InvalidArgument);

    auto invalidFile = lc::SpdLogger::rotatingFile(lc::SpdLogger::RotatingFileOptions {});
    assert(!invalidFile.isOk());
    assert(invalidFile.status().code() == lc::StatusCode::InvalidArgument);

    auto createdConsole = lc::SpdLogger::console(lc::SpdLogger::ConsoleOptions {
        .name_ = "langgraph_cpp_logging_test_console",
        .async_ = lc::SpdLogger::AsyncOptions {
            .queueSize_ = 64,
            .workerThreads_ = 1,
        },
    });
    assert(createdConsole.isOk());

    auto conflictingConsole = lc::SpdLogger::console(lc::SpdLogger::ConsoleOptions {
        .name_ = "langgraph_cpp_logging_test_conflict",
        .async_ = lc::SpdLogger::AsyncOptions {
            .queueSize_ = 128,
            .workerThreads_ = 1,
        },
    });
    assert(!conflictingConsole.isOk());
    assert(conflictingConsole.status().code() == lc::StatusCode::FailedPrecondition);
#endif

    return 0;
}
