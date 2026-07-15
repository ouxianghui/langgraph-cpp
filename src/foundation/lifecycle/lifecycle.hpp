#pragma once

#include "foundation/logging/logger.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace lgc {

enum class LifecycleState : std::uint8_t {
    Created,
    Starting,
    Started,
    Closing,
    Closed,
    Failed,
};

struct CloseOptions {
    Clock::Duration timeout_ { std::chrono::seconds(5) };
    bool continueOnError_ { true };
};

struct ComponentStatus {
    std::string name_;
    LifecycleState state_ { LifecycleState::Created };
    bool closed_ { false };
    Status status_ { Status::ok() };
};

class ILifecycle {
public:
    virtual ~ILifecycle() = default;

    ILifecycle(const ILifecycle&) = delete;
    ILifecycle& operator=(const ILifecycle&) = delete;
    ILifecycle(ILifecycle&&) = delete;
    ILifecycle& operator=(ILifecycle&&) = delete;

protected:
    ILifecycle() = default;

public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual Status start() = 0;
    [[nodiscard]] virtual Status waitIdle(Clock::Duration timeout) = 0;
    [[nodiscard]] virtual Status close(Clock::Duration timeout) = 0;
    [[nodiscard]] virtual bool isClosed() const noexcept = 0;
};

/// Reusable ILifecycle base that carries a non-empty component name.
/// Adapter authors can derive from this instead of reimplementing name().
class NamedLifecycleComponent : public ILifecycle {
public:
    explicit NamedLifecycleComponent(std::string name);

    [[nodiscard]] std::string_view name() const noexcept override;

private:
    std::string name_;
};

class Lifecycle final {
public:
    explicit Lifecycle(std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    ~Lifecycle();

    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;
    Lifecycle(Lifecycle&&) = delete;
    Lifecycle& operator=(Lifecycle&&) = delete;

    [[nodiscard]] Status add(std::shared_ptr<ILifecycle> component);

    [[nodiscard]] Status start();
    [[nodiscard]] Status waitIdle(Clock::Duration timeout);
    [[nodiscard]] Status close(CloseOptions options = {});
    [[nodiscard]] bool isClosed() const noexcept;

    [[nodiscard]] LifecycleState state() const noexcept;
    [[nodiscard]] std::vector<ComponentStatus> components() const;
    [[nodiscard]] std::size_t count() const noexcept;

private:
    struct ComponentRecord {
        std::shared_ptr<ILifecycle> component_;
        LifecycleState state_ { LifecycleState::Created };
        Status status_ { Status::ok() };
    };

    mutable std::mutex mutex_;
    std::condition_variable stateCv_;
    std::vector<ComponentRecord> components_;
    std::shared_ptr<ILogger> logger_;
    LifecycleState state_ { LifecycleState::Created };
    bool closing_ { false };
    bool closeAttempted_ { false };
    Status startStatus_ { Status::ok() };
    Status closeStatus_ { Status::ok() };
};

[[nodiscard]] std::string_view stateName(LifecycleState state) noexcept;

} // namespace lgc
