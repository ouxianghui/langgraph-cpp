#pragma once

#include "foundation/process/process.hpp"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace lgc::process_detail {

[[nodiscard]] std::string lowerAscii(std::string_view value);
void appendBounded(std::string& out, bool& truncated, const char* data, std::size_t size, std::size_t limit);
[[nodiscard]] Status statusFromExitCode(int exitCode);
[[nodiscard]] Status statusFromErrno(int error, std::string_view operation);
[[nodiscard]] Status requireStdinWithinLimit(std::size_t current, std::size_t next, std::size_t limit);
[[nodiscard]] bool hasStreamingStdin(const ProcessOptions& options) noexcept;

template <typename Write>
[[nodiscard]] Status streamProcessStdin(const ProcessOptions& options, Write&& write)
{
    std::size_t written = 0;
    auto writeChunk = [&](std::string_view chunk) -> Status {
        if (auto status = requireStdinWithinLimit(written, chunk.size(), options.maxStdinBytes_); !status.isOk())
            return status;
        if (auto status = write(chunk); !status.isOk())
            return status;
        written += chunk.size();
        return Status::ok();
    };

    if (options.stdin_)
        return writeChunk(*options.stdin_);

    while (options.stdinProvider_) {
        Result<std::string> chunk = Status::internal("process stdin provider did not return a value");
        try {
            chunk = options.stdinProvider_();
        } catch (const std::exception& error) {
            return Status::internal(std::string("process stdin provider threw: ") + error.what());
        } catch (...) {
            return Status::internal("process stdin provider threw");
        }

        if (!chunk.isOk())
            return chunk.status();
        if (chunk->empty())
            break;
        if (auto status = writeChunk(*chunk); !status.isOk())
            return status;
    }
    return Status::ok();
}

} // namespace lgc::process_detail
