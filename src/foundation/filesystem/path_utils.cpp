#include "foundation/filesystem/filesystem.hpp"

#include "foundation/filesystem/filesystem_common.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>
#include <utility>

namespace lc {
namespace fs = std::filesystem;

namespace filesystem_detail {

[[nodiscard]] Result<fs::path> absoluteNormalized(const fs::path& input)
{
    std::error_code ec;
    auto absolute = fs::absolute(input, ec);
    if (ec) {
        std::string message("failed to resolve absolute path: ");
        message.append(ec.message());
        return Status::invalidArgument(std::move(message));
    }

    auto normalized = fs::weakly_canonical(absolute, ec);
    if (ec)
        normalized = absolute.lexically_normal();
    return normalized.lexically_normal();
}

[[nodiscard]] bool pathStartsWith(const fs::path& base, const fs::path& candidate)
{
    auto baseIt = base.begin();
    auto candidateIt = candidate.begin();
    for (; baseIt != base.end(); ++baseIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *baseIt != *candidateIt)
            return false;
    }
    return true;
}

[[nodiscard]] std::string upperAscii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char ch : value)
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    return out;
}

[[nodiscard]] bool containsPathSeparator(std::string_view value)
{
    return value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos;
}

[[nodiscard]] bool reservedWindowsName(std::string_view component)
{
    const auto dot = component.find('.');
    auto base = component.substr(0, dot);
    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
        base.remove_suffix(1);
    const auto upper = upperAscii(base);

    static constexpr std::array<std::string_view, 4> reserved {
        "CON",
        "PRN",
        "AUX",
        "NUL",
    };
    if (std::find(reserved.begin(), reserved.end(), upper) != reserved.end())
        return true;

    if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT"))) {
        const char suffix = upper[3];
        return suffix >= '1' && suffix <= '9';
    }

    return false;
}

[[nodiscard]] Status validatePathComponent(
    std::string_view component,
    std::string_view field,
    const PathPolicy& policy)
{
    if (component.empty()) {
        std::string message("path component cannot be empty: ");
        message.append(field);
        return Status::invalidArgument(std::move(message));
    }

    if (component.size() > policy.maxComponentLength_) {
        std::string message("path component is too long: ");
        message.append(field);
        return Status::invalidArgument(std::move(message));
    }

    if (containsPathSeparator(component)) {
        std::string message("path component cannot contain separators: ");
        message.append(field);
        return Status::invalidArgument(std::move(message));
    }

    if (policy.rejectReservedNames_ && reservedWindowsName(component)) {
        std::string message("path component uses a reserved name: ");
        message.append(field);
        return Status::invalidArgument(std::move(message));
    }

    for (const char ch : component) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) {
            std::string message("path component contains a control character: ");
            message.append(field);
            return Status::invalidArgument(std::move(message));
        }

        if (!policy.allowedChars_.empty()
            && policy.allowedChars_.find(ch) == std::string::npos) {
            std::string message("path component contains a disallowed character: ");
            message.append(field);
            return Status::invalidArgument(std::move(message));
        }
    }

    return Status::ok();
}

[[nodiscard]] Status validateRelativePathPolicy(const fs::path& relativePath, const PathPolicy& policy)
{
    const auto generic = relativePath.generic_string();
    if (generic.size() > policy.maxPathLength_)
        return Status::invalidArgument("relative path is too long");

    for (const auto& part : relativePath) {
        const auto component = part.generic_string();
        if (component == ".")
            continue;
        if (auto status = validatePathComponent(component, "relative_path", policy); !status.isOk())
            return status;
    }

    return Status::ok();
}

} // namespace filesystem_detail

namespace {
using filesystem_detail::absoluteNormalized;
using filesystem_detail::pathStartsWith;
using filesystem_detail::validateRelativePathPolicy;
}

Result<std::filesystem::path> normalize(
    const std::filesystem::path& path,
    const std::filesystem::path& baseDirectory)
{
    if (path.empty())
        return Status::invalidArgument("path cannot be empty");

    fs::path resolved = path;
    if (resolved.is_relative()) {
        resolved = baseDirectory.empty() ? fs::current_path() / resolved : baseDirectory / resolved;
    }
    return absoluteNormalized(resolved);
}

Result<std::filesystem::path> realPath(
    const std::filesystem::path& path,
    const std::filesystem::path& baseDirectory)
{
    auto normalized = normalize(path, baseDirectory);
    if (!normalized.isOk())
        return normalized.status();

    std::error_code ec;
    if (!fs::exists(*normalized, ec) || ec) {
        std::string message("path does not exist: ");
        message.append(normalized->string());
        return Status::notFound(std::move(message));
    }
    return normalized;
}

Result<bool> isInside(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate)
{
    auto root = normalize(directory);
    if (!root.isOk())
        return root.status();

    auto child = normalize(candidate);
    if (!child.isOk())
        return child.status();

    return pathStartsWith(*root, *child);
}

Status requireInside(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate)
{
    auto result = isInside(directory, candidate);
    if (!result.isOk())
        return result.status();
    if (!*result)
        return Status::permissionDenied("path is outside the allowed directory");
    return Status::ok();
}

Status requireSafeRelativePath(const std::filesystem::path& relativePath, const PathPolicy& policy)
{
    if (relativePath.empty())
        return Status::invalidArgument("relative path cannot be empty");
    if (relativePath.is_absolute())
        return Status::invalidArgument("path must be relative");

    const auto normalized = relativePath.lexically_normal();
    if (normalized.empty() || normalized == ".")
        return Status::invalidArgument("relative path cannot resolve to current directory");

    for (const auto& part : normalized) {
        if (part == "..")
            return Status::permissionDenied("relative path cannot contain '..'");
    }

    if (auto status = validateRelativePathPolicy(normalized, policy); !status.isOk())
        return status;

    return Status::ok();
}

Result<std::filesystem::path> resolveChild(
    const std::filesystem::path& directory,
    const std::filesystem::path& relativePath,
    const PathPolicy& policy)
{
    if (auto status = requireSafeRelativePath(relativePath, policy); !status.isOk())
        return status;

    auto resolved = normalize(relativePath, directory);
    if (!resolved.isOk())
        return resolved.status();

    if (auto status = requireInside(directory, *resolved); !status.isOk())
        return status;

    return resolved;
}

Result<void> ensureDir(const std::filesystem::path& directory)
{
    if (directory.empty())
        return Status::invalidArgument("directory path cannot be empty");

    std::error_code ec;
    if (fs::exists(directory, ec)) {
        if (ec) {
            std::string message("failed to check directory: ");
            message.append(ec.message());
            return Status::internal(std::move(message));
        }
        if (!fs::is_directory(directory, ec))
            return Status::failedPrecondition("path exists but is not a directory");
        return okResult();
    }

    if (!fs::create_directories(directory, ec) && ec) {
        std::string message("failed to create directory: ");
        message.append(ec.message());
        return Status::internal(std::move(message));
    }
    return okResult();
}

Result<std::string> readFile(const std::filesystem::path& path, const ReadFileOptions& options)
{
    if (path.empty())
        return Status::invalidArgument("path cannot be empty");

    std::error_code ec;
    if (fs::is_directory(path, ec) && !ec)
        return Status::failedPrecondition("path is a directory");

    const auto size = fs::file_size(path, ec);
    if (!ec && options.maxBytes_ != 0U && size > options.maxBytes_)
        return Status::resourceExhausted("file exceeds max read bytes");

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::string message("failed to open file for read: ");
        message.append(path.string());
        return Status::notFound(std::move(message));
    }

    std::string text;
    if (!ec && size > 0)
        text.reserve(static_cast<std::size_t>(size));

    std::array<char, 8192> buffer {};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count <= 0)
            break;

        const auto chunk = static_cast<std::size_t>(count);
        if (options.maxBytes_ != 0U && text.size() + chunk > options.maxBytes_)
            return Status::resourceExhausted("file exceeds max read bytes");

        text.append(buffer.data(), chunk);
    }

    if (!file.eof() && file.fail()) {
        std::string message("failed to read file: ");
        message.append(path.string());
        return Status::internal(std::move(message));
    }
    return text;
}

} // namespace lc
