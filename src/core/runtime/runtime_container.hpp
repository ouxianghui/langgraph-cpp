#pragma once

#include "core/runtime/runtime_services.hpp"
#include "foundation/lifecycle/lifecycle.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <memory>
#include <mutex>

namespace lc {

/// Runtime-facing service owner. Prefer this container plus `Lifecycle` for runtime integration;
/// low-level `threading` / `timer` objects should be owned behind executors and schedulers.
struct RuntimeContainerOptions {
    RuntimeServiceRequirements requirements_ { RuntimeServiceRequirements::core() };
    CloseOptions closeOptions_;
};

class RuntimeContainer final {
public:
    explicit RuntimeContainer(
        RuntimeServices services,
        RuntimeContainerOptions options = {});
    ~RuntimeContainer();

    RuntimeContainer(const RuntimeContainer&) = delete;
    RuntimeContainer& operator=(const RuntimeContainer&) = delete;
    RuntimeContainer(RuntimeContainer&&) = delete;
    RuntimeContainer& operator=(RuntimeContainer&&) = delete;

    [[nodiscard]] const RuntimeServices& services() const noexcept;
    [[nodiscard]] RuntimeServices& services() noexcept;

    [[nodiscard]] Status validate() const;
    [[nodiscard]] Status start();
    [[nodiscard]] Status waitIdle(Clock::Duration timeout);
    [[nodiscard]] Status close(CloseOptions options = {});
    [[nodiscard]] bool isClosed() const noexcept;
    [[nodiscard]] std::shared_ptr<Lifecycle> lifecycle() const;

private:
    [[nodiscard]] Status ensureLifecycleLocked();

    mutable std::mutex mutex_;
    RuntimeServices services_;
    RuntimeContainerOptions options_;
    std::shared_ptr<Lifecycle> lifecycle_;
    bool closed_ { false };
};

[[nodiscard]] Result<std::shared_ptr<RuntimeContainer>> createRuntimeContainer(
    RuntimeServices services,
    RuntimeContainerOptions options = {});

} // namespace lc
