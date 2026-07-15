#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/process/process.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

lgc::ProcessOptions shell(std::string script)
{
#if defined(_WIN32)
    return lgc::ProcessOptions {
        .executable_ = "cmd.exe",
        .arguments_ = { "/C", std::move(script) },
        .shellAllowed_ = true,
    };
#else
    return lgc::ProcessOptions {
        .executable_ = "/bin/sh",
        .arguments_ = { "-c", std::move(script) },
        .shellAllowed_ = true,
    };
#endif
}

lgc::ProcessOptions catCommand()
{
#if defined(_WIN32)
    return lgc::ProcessOptions {
        .executable_ = "cmd.exe",
        .arguments_ = { "/C", "more" },
        .shellAllowed_ = true,
    };
#else
    return lgc::ProcessOptions {
        .executable_ = "/bin/cat",
    };
#endif
}

std::string sleepCommand()
{
#if defined(_WIN32)
    return "ping -n 5 127.0.0.1 > nul";
#else
    return "sleep 5";
#endif
}

} // namespace

int main()
{
    using namespace std::chrono_literals;

    {
#if defined(_WIN32)
        auto options = shell("echo out && echo err 1>&2 && exit /b 7");
#else
        auto options = shell("printf out; printf err >&2; exit 7");
#endif
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->exited_);
        assert(result->exitCode_ == 7);
        assert(result->status_.code() == lgc::StatusCode::Unknown);
        assert(result->stdout_.find("out") != std::string::npos);
        assert(result->stderr_.find("err") != std::string::npos);
    }

    {
#if defined(_WIN32)
        auto rejected = lgc::validateProcessOptions(lgc::ProcessOptions {
            .executable_ = "cmd.exe",
            .arguments_ = { "/C", "echo rejected" },
        });
#else
        auto rejected = lgc::validateProcessOptions(lgc::ProcessOptions {
            .executable_ = "/bin/sh",
            .arguments_ = { "-c", "echo rejected" },
        });
#endif
        assert(rejected.code() == lgc::StatusCode::InvalidArgument);
    }

    {
#if defined(_WIN32)
        auto options = shell("echo %LC_PROCESS_TEST%");
#else
        auto options = shell("printf %s \"$LC_PROCESS_TEST\"");
#endif
        options.environment_["LC_PROCESS_TEST"] = "env-value";
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->success());
        assert(result->stdout_.find("env-value") != std::string::npos);
    }

#if !defined(_WIN32)
    {
        setenv("LC_PROCESS_ISOLATION_TEST", "visible", 1);
        auto options = shell(R"(printf %s "${LC_PROCESS_ISOLATION_TEST:-missing}")");
        options.inheritEnvironment_ = false;
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->success());
        assert(result->stdout_ == "missing");
        unsetenv("LC_PROCESS_ISOLATION_TEST");
    }
#endif

    {
        auto options = catCommand();
        options.stdin_ = "hello stdin\n";
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->success());
        assert(result->stdout_.find("hello stdin") != std::string::npos);
    }

    {
        auto options = catCommand();
        int chunk = 0;
        options.stdinProvider_ = [&chunk]() -> lgc::Result<std::string> {
            if (chunk == 0) {
                ++chunk;
                return std::string("streamed ");
            }
            if (chunk == 1) {
                ++chunk;
                return std::string("stdin\n");
            }
            return std::string();
        };
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->success());
        assert(result->stdout_.find("streamed stdin") != std::string::npos);
    }

    {
        auto options = catCommand();
        options.stdin_ = std::string(256 * 1024, 'x');
        options.maxStdoutBytes_ = options.stdin_->size();
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->success());
        assert(result->stdout_.size() == options.stdin_->size());
    }

    {
        auto options = catCommand();
        options.stdinProvider_ = []() -> lgc::Result<std::string> {
            return lgc::Status::internal("stdin source failed");
        };
        auto result = lgc::runProcess(options);
        assert(!result.isOk());
        assert(result.status().code() == lgc::StatusCode::Internal);
    }

    {
        auto options = shell(sleepCommand());
        options.timeout_ = 50ms;
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->timedOut_);
        assert(result->status_.code() == lgc::StatusCode::DeadlineExceeded);
    }

#if !defined(_WIN32)
    {
        auto options = shell("sleep 2 & printf done");
        const auto started = std::chrono::steady_clock::now();
        auto result = lgc::runProcess(options);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        assert(result.isOk());
        assert(result->success());
        assert(result->stdout_.find("done") != std::string::npos);
        assert(elapsed < 1500ms);
    }
#endif

#if !defined(_WIN32)
    {
        const auto marker = std::filesystem::temp_directory_path() / "langgraph_cpp_process_tree_leak";
        std::filesystem::remove(marker);
        auto options = shell("(sleep 1; printf leaked > \"" + marker.string() + "\") & wait");
        options.timeout_ = 50ms;
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->timedOut_);
        std::this_thread::sleep_for(1200ms);
        assert(!std::filesystem::exists(marker));
    }
#endif

    {
        lgc::CancellationSource source;
        auto options = shell(sleepCommand());
        options.cancellation_ = source.token();

        std::thread cancelThread([&source] {
            std::this_thread::sleep_for(50ms);
            source.cancel("stop process");
        });

        auto result = lgc::runProcess(options);
        cancelThread.join();

        assert(result.isOk());
        assert(result->cancelled_);
        assert(result->status_.code() == lgc::StatusCode::Cancelled);
    }

    {
        auto result = lgc::runProcess(lgc::ProcessOptions {
            .executable_ = "langgraph-cpp-command-that-does-not-exist",
        });
        assert(!result.isOk());
        assert(result.status().code() == lgc::StatusCode::NotFound);
    }

    {
        auto invalid = lgc::validateProcessOptions(lgc::ProcessOptions {});
        assert(invalid.code() == lgc::StatusCode::InvalidArgument);
    }

    {
        auto options = shell(
#if defined(_WIN32)
            "echo abcdef && echo ghijkl 1>&2"
#else
            "printf abcdef; printf ghijkl >&2"
#endif
        );
        options.maxStdoutBytes_ = 3;
        options.maxStderrBytes_ = 2;
        auto result = lgc::runProcess(options);
        assert(result.isOk());
        assert(result->stdoutTruncated_);
        assert(result->stderrTruncated_);
        assert(result->stdout_.size() == 3);
        assert(result->stderr_.size() == 2);
    }

    {
        auto options = shell("echo bad");
        options.timeout_ = 0ms;
        assert(lgc::validateProcessOptions(options).code() == lgc::StatusCode::InvalidArgument);

        options.timeout_.reset();
        options.environment_["BAD=NAME"] = "x";
        assert(lgc::validateProcessOptions(options).code() == lgc::StatusCode::InvalidArgument);

        options.environment_.clear();
        options.environment_["OK_NAME"] = std::string("bad\0value", 9);
        assert(lgc::validateProcessOptions(options).code() == lgc::StatusCode::InvalidArgument);

        options.environment_.clear();
        options.stdin_ = "12345";
        options.maxStdinBytes_ = 4;
        assert(lgc::validateProcessOptions(options).code() == lgc::StatusCode::ResourceExhausted);

        options.maxStdinBytes_ = 1024;
        options.stdinProvider_ = []() -> lgc::Result<std::string> { return std::string(); };
        assert(lgc::validateProcessOptions(options).code() == lgc::StatusCode::InvalidArgument);
    }

    {
        auto options = shell("echo cwd");
        options.workingDirectory_ = std::filesystem::temp_directory_path() / "langgraph_cpp_missing_cwd";
        auto result = lgc::runProcess(options);
        assert(!result.isOk());
        assert(result.status().code() == lgc::StatusCode::NotFound);
    }

    {
        std::atomic<int> successes { 0 };
        std::vector<std::thread> workers;
        workers.reserve(4);
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&] {
                auto result = lgc::runProcess(shell("echo concurrent"));
                if (result.isOk() && result->success())
                    successes.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(successes.load(std::memory_order_relaxed) == 4);
    }

    return 0;
}
