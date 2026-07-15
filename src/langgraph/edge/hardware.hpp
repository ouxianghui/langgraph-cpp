#pragma once

#include "foundation/status/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

namespace lgc {

using HardwareBytes = std::vector<std::uint8_t>;

enum class GpioDirection : std::uint8_t {
    Input,
    Output,
};

enum class GpioLevel : std::uint8_t {
    Low,
    High,
};

struct GpioPinConfig {
    std::string line_;
    GpioDirection direction_ { GpioDirection::Input };
    std::optional<GpioLevel> initialLevel_;
};

class IGpioAdapter {
public:
    virtual ~IGpioAdapter() = default;

    [[nodiscard]] virtual Result<void> configurePin(const GpioPinConfig& config) = 0;
    [[nodiscard]] virtual Result<GpioLevel> readPin(std::string line) = 0;
    [[nodiscard]] virtual Result<void> writePin(std::string line, GpioLevel level) = 0;
};

struct SysfsGpioAdapterOptions {
    /// Root directory for Linux sysfs GPIO. Tests may point this at a temporary fake sysfs tree.
    std::filesystem::path root_ { "/sys/class/gpio" };
    /// When true, configurePin writes the numeric line id to root_/export if the gpio directory is missing.
    bool autoExport_ { false };
};

/// Concrete GPIO adapter for Linux sysfs-style GPIO directories.
class SysfsGpioAdapter final : public IGpioAdapter {
public:
    explicit SysfsGpioAdapter(SysfsGpioAdapterOptions options = {});

    [[nodiscard]] Result<void> configurePin(const GpioPinConfig& config) override;
    [[nodiscard]] Result<GpioLevel> readPin(std::string line) override;
    [[nodiscard]] Result<void> writePin(std::string line, GpioLevel level) override;

private:
    [[nodiscard]] Result<std::filesystem::path> lineDirectory(std::string_view line) const;
    [[nodiscard]] Result<void> ensureExported(std::string_view line) const;

    SysfsGpioAdapterOptions options_;
};

enum class UartParity : std::uint8_t {
    None,
    Even,
    Odd,
};

struct UartConfig {
    std::string port_;
    std::uint32_t baudRate_ { 115200 };
    std::uint8_t dataBits_ { 8 };
    std::uint8_t stopBits_ { 1 };
    UartParity parity_ { UartParity::None };
};

class IUartAdapter {
public:
    virtual ~IUartAdapter() = default;

    [[nodiscard]] virtual Result<void> open(const UartConfig& config) = 0;
    [[nodiscard]] virtual Result<void> close(std::string port) = 0;
    [[nodiscard]] virtual Result<void> write(std::string port, HardwareBytes data) = 0;
    [[nodiscard]] virtual Result<HardwareBytes> read(std::string port, std::size_t maxBytes) = 0;
};

struct I2cTransfer {
    std::string bus_;
    std::uint16_t address_ { 0 };
    HardwareBytes write_;
    std::size_t readLength_ { 0 };
};

class II2cAdapter {
public:
    virtual ~II2cAdapter() = default;

    [[nodiscard]] virtual Result<void> write(std::string bus, std::uint16_t address, HardwareBytes data) = 0;
    [[nodiscard]] virtual Result<HardwareBytes> read(std::string bus, std::uint16_t address, std::size_t length) = 0;
    [[nodiscard]] virtual Result<HardwareBytes> transfer(const I2cTransfer& transfer) = 0;
};

struct CanFrame {
    std::string bus_;
    std::uint32_t id_ { 0 };
    bool extended_ { false };
    bool fd_ { false };
    HardwareBytes data_;
};

class ICanAdapter {
public:
    virtual ~ICanAdapter() = default;

    [[nodiscard]] virtual Result<void> send(CanFrame frame) = 0;
    [[nodiscard]] virtual Result<CanFrame> receive(std::string bus, std::uint32_t timeoutMs) = 0;
};

struct Ros2ServiceRequest {
    std::string service_;
    std::string type_;
    nlohmann::json request_ { nlohmann::json::object() };
    std::uint32_t timeoutMs_ { 1000 };
};

struct Ros2ActionGoal {
    std::string action_;
    std::string type_;
    nlohmann::json goal_ { nlohmann::json::object() };
    std::uint32_t timeoutMs_ { 1000 };
};

class IRos2Adapter {
public:
    virtual ~IRos2Adapter() = default;

    [[nodiscard]] virtual Result<nlohmann::json> callService(Ros2ServiceRequest request) = 0;
    [[nodiscard]] virtual Result<nlohmann::json> sendActionGoal(Ros2ActionGoal goal) = 0;
};

struct EdgeAdapterCapabilities {
    bool gpio_ { false };
    bool uart_ { false };
    bool i2c_ { false };
    bool can_ { false };
    bool ros2_ { false };

    [[nodiscard]] bool any() const noexcept
    {
        return gpio_ || uart_ || i2c_ || can_ || ros2_;
    }
};

/// Thread-safe registry for concrete edge adapters supplied by an application.
class EdgeAdapterRegistry final {
public:
    template <typename Adapter>
    void set(std::shared_ptr<Adapter> adapter)
    {
        std::lock_guard lock(mutex_);
        setLocked(std::move(adapter));
    }

    template <typename Adapter>
    [[nodiscard]] std::shared_ptr<Adapter> find() const
    {
        std::lock_guard lock(mutex_);
        return slot<Adapter>();
    }

    template <typename Adapter>
    [[nodiscard]] Result<std::shared_ptr<Adapter>> require() const
    {
        auto adapter = find<Adapter>();
        if (!adapter)
            return Status::notFound(std::string(adapterName<Adapter>()) + " edge adapter is not registered");
        return adapter;
    }

    [[nodiscard]] EdgeAdapterCapabilities capabilities() const
    {
        std::lock_guard lock(mutex_);
        return EdgeAdapterCapabilities {
            .gpio_ = static_cast<bool>(gpio_),
            .uart_ = static_cast<bool>(uart_),
            .i2c_ = static_cast<bool>(i2c_),
            .can_ = static_cast<bool>(can_),
            .ros2_ = static_cast<bool>(ros2_),
        };
    }

private:
    template <typename>
    static constexpr bool kAlwaysFalse = false;

    template <typename Adapter>
    static constexpr std::string_view adapterName() noexcept
    {
        if constexpr (std::is_same_v<Adapter, IGpioAdapter>) {
            return "GPIO";
        } else if constexpr (std::is_same_v<Adapter, IUartAdapter>) {
            return "UART";
        } else if constexpr (std::is_same_v<Adapter, II2cAdapter>) {
            return "I2C";
        } else if constexpr (std::is_same_v<Adapter, ICanAdapter>) {
            return "CAN";
        } else if constexpr (std::is_same_v<Adapter, IRos2Adapter>) {
            return "ROS2";
        } else {
            static_assert(kAlwaysFalse<Adapter>, "unsupported edge adapter interface");
        }
    }

    template <typename Adapter>
    [[nodiscard]] const std::shared_ptr<Adapter>& slot() const
    {
        if constexpr (std::is_same_v<Adapter, IGpioAdapter>) {
            return gpio_;
        } else if constexpr (std::is_same_v<Adapter, IUartAdapter>) {
            return uart_;
        } else if constexpr (std::is_same_v<Adapter, II2cAdapter>) {
            return i2c_;
        } else if constexpr (std::is_same_v<Adapter, ICanAdapter>) {
            return can_;
        } else if constexpr (std::is_same_v<Adapter, IRos2Adapter>) {
            return ros2_;
        } else {
            static_assert(kAlwaysFalse<Adapter>, "unsupported edge adapter interface");
        }
    }

    template <typename Adapter>
    void setLocked(std::shared_ptr<Adapter> adapter)
    {
        if constexpr (std::is_base_of_v<IGpioAdapter, Adapter>) {
            gpio_ = std::move(adapter);
        } else if constexpr (std::is_base_of_v<IUartAdapter, Adapter>) {
            uart_ = std::move(adapter);
        } else if constexpr (std::is_base_of_v<II2cAdapter, Adapter>) {
            i2c_ = std::move(adapter);
        } else if constexpr (std::is_base_of_v<ICanAdapter, Adapter>) {
            can_ = std::move(adapter);
        } else if constexpr (std::is_base_of_v<IRos2Adapter, Adapter>) {
            ros2_ = std::move(adapter);
        } else {
            static_assert(kAlwaysFalse<Adapter>, "unsupported edge adapter interface");
        }
    }

    mutable std::mutex mutex_;
    std::shared_ptr<IGpioAdapter> gpio_;
    std::shared_ptr<IUartAdapter> uart_;
    std::shared_ptr<II2cAdapter> i2c_;
    std::shared_ptr<ICanAdapter> can_;
    std::shared_ptr<IRos2Adapter> ros2_;
};

} // namespace lgc
