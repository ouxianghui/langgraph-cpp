#include "langgraph/edge/hardware.hpp"

#include <fstream>
#include <string_view>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] bool validLineName(std::string_view line)
{
    if (line.empty())
        return false;
    return line.find('/') == std::string_view::npos
        && line.find('\\') == std::string_view::npos
        && line.find("..") == std::string_view::npos;
}

[[nodiscard]] std::string exportName(std::string_view line)
{
    constexpr std::string_view prefix = "gpio";
    if (line.starts_with(prefix) && line.size() > prefix.size())
        return std::string(line.substr(prefix.size()));
    return std::string(line);
}

[[nodiscard]] Result<void> writeText(const std::filesystem::path& path, std::string_view value)
{
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out)
        return Status::notFound("failed to open GPIO sysfs file for write: " + path.string());
    out << value;
    if (!out)
        return Status::internal("failed to write GPIO sysfs file: " + path.string());
    return okResult();
}

[[nodiscard]] Result<std::string> readText(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
        return Status::notFound("failed to open GPIO sysfs file for read: " + path.string());

    std::string value;
    in >> value;
    if (!in && value.empty())
        return Status::internal("failed to read GPIO sysfs file: " + path.string());
    return value;
}

[[nodiscard]] std::string directionName(GpioDirection direction)
{
    return direction == GpioDirection::Output ? "out" : "in";
}

[[nodiscard]] std::string levelName(GpioLevel level)
{
    return level == GpioLevel::High ? "1" : "0";
}

} // namespace

SysfsGpioAdapter::SysfsGpioAdapter(SysfsGpioAdapterOptions options)
    : options_(std::move(options))
{
}

Result<void> SysfsGpioAdapter::configurePin(const GpioPinConfig& config)
{
    auto directory = lineDirectory(config.line_);
    if (!directory.isOk())
        return directory.status();
    if (!std::filesystem::exists(*directory)) {
        if (auto status = ensureExported(config.line_); !status.isOk())
            return status;
    }
    if (!std::filesystem::exists(*directory))
        return Status::notFound("GPIO sysfs line directory is missing: " + directory->string());

    if (auto status = writeText(*directory / "direction", directionName(config.direction_)); !status.isOk())
        return status;
    if (config.initialLevel_.has_value()) {
        if (auto status = writePin(config.line_, *config.initialLevel_); !status.isOk())
            return status;
    }
    return okResult();
}

Result<GpioLevel> SysfsGpioAdapter::readPin(std::string line)
{
    auto directory = lineDirectory(line);
    if (!directory.isOk())
        return directory.status();

    auto value = readText(*directory / "value");
    if (!value.isOk())
        return value.status();
    if (*value == "1")
        return GpioLevel::High;
    if (*value == "0")
        return GpioLevel::Low;
    return Status::invalidArgument("GPIO sysfs value must be 0 or 1: " + *value);
}

Result<void> SysfsGpioAdapter::writePin(std::string line, GpioLevel level)
{
    auto directory = lineDirectory(line);
    if (!directory.isOk())
        return directory.status();
    return writeText(*directory / "value", levelName(level));
}

Result<std::filesystem::path> SysfsGpioAdapter::lineDirectory(std::string_view line) const
{
    if (!validLineName(line))
        return Status::invalidArgument("GPIO line name must be non-empty and path-safe");
    if (options_.root_.empty())
        return Status::invalidArgument("GPIO sysfs root cannot be empty");
    return options_.root_ / std::string(line);
}

Result<void> SysfsGpioAdapter::ensureExported(std::string_view line) const
{
    if (!options_.autoExport_)
        return okResult();
    return writeText(options_.root_ / "export", exportName(line));
}

} // namespace lgc
