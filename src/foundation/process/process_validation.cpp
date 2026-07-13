#include "foundation/process/process.hpp"

#include "foundation/process/process_common.hh"

#include <string>
#include <string_view>

namespace lc::process_detail {

std::string lowerAscii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        if (ch >= 'A' && ch <= 'Z')
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        else
            out.push_back(ch);
    }
    return out;
}

} // namespace lc::process_detail

namespace lc {
namespace {

[[nodiscard]] bool isValidEnvironmentName(std::string_view name) noexcept
{
    return !name.empty() && name.find('=') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

[[nodiscard]] std::string executableName(std::string_view executable)
{
    const auto slash = executable.find_last_of("/\\");
    const auto name = slash == std::string_view::npos ? executable : executable.substr(slash + 1);
    return process_detail::lowerAscii(name);
}

[[nodiscard]] bool isShellExecutable(std::string_view executable)
{
    const auto name = executableName(executable);
    return name == "sh" || name == "bash" || name == "dash" || name == "zsh" || name == "ksh"
        || name == "cmd" || name == "cmd.exe" || name == "powershell" || name == "powershell.exe"
        || name == "pwsh" || name == "pwsh.exe";
}

} // namespace

Status validateProcessOptions(const ProcessOptions& options)
{
    if (options.executable_.empty())
        return Status::invalidArgument("process executable cannot be empty");
    if (options.executable_.find('\0') != std::string::npos)
        return Status::invalidArgument("process executable contains a null byte");
    if (!options.shellAllowed_ && isShellExecutable(options.executable_))
        return Status::invalidArgument("shell process execution requires shellAllowed");
    for (const auto& arg : options.arguments_) {
        if (arg.find('\0') != std::string::npos)
            return Status::invalidArgument("process argument contains a null byte");
    }
    if (options.workingDirectory_ && options.workingDirectory_->string().find('\0') != std::string::npos)
        return Status::invalidArgument("process working directory contains a null byte");
    for (const auto& [name, value] : options.environment_) {
        if (!isValidEnvironmentName(name))
            return Status::invalidArgument("process environment variable name is invalid");
        if (value.find('\0') != std::string::npos)
            return Status::invalidArgument("process environment variable value contains a null byte");
    }
    if (options.stdin_ && options.stdinProvider_)
        return Status::invalidArgument("process stdin and stdinProvider cannot both be set");
    if (options.stdin_) {
        if (auto status = process_detail::requireStdinWithinLimit(0, options.stdin_->size(), options.maxStdinBytes_);
            !status.isOk())
            return status;
    }
    if (options.timeout_ && *options.timeout_ <= Clock::Duration::zero())
        return Status::invalidArgument("process timeout must be positive");
    return Status::ok();
}

} // namespace lc
