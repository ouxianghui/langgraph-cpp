#include "foundation/process/process_common.hh"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace lgc::process_detail {

void appendBounded(std::string& out, bool& truncated, const char* data, std::size_t size, std::size_t limit)
{
    if (size == 0 || limit == 0) {
        if (size != 0)
            truncated = true;
        return;
    }

    if (out.size() >= limit) {
        truncated = true;
        return;
    }

    const auto available = limit - out.size();
    const auto count = std::min(size, available);
    out.append(data, count);
    if (count != size)
        truncated = true;
}

Status statusFromExitCode(int exitCode)
{
    if (exitCode == 0)
        return Status::ok();
    return Status::unknown("process exited with code " + std::to_string(exitCode));
}

Status statusFromErrno(int error, std::string_view operation)
{
    std::string message(operation);
    message.append(": ");
    message.append(std::strerror(error));
    if (error == ENOENT || error == ENOTDIR)
        return Status::notFound(std::move(message));
    if (error == EACCES || error == EPERM)
        return Status::permissionDenied(std::move(message));
    return Status::internal(std::move(message));
}

Status requireStdinWithinLimit(std::size_t current, std::size_t next, std::size_t limit)
{
    if (limit == 0 && next != 0)
        return Status::resourceExhausted("process stdin exceeds configured maximum size");
    if (limit != 0 && (current > limit || next > limit - current))
        return Status::resourceExhausted("process stdin exceeds configured maximum size");
    return Status::ok();
}

bool hasStreamingStdin(const ProcessOptions& options) noexcept
{
    return options.stdin_.has_value() || static_cast<bool>(options.stdinProvider_);
}

} // namespace lgc::process_detail
