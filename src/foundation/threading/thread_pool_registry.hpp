#pragma once

#include "foundation/threading/thread_pool.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lgc {

/// Owns named `ThreadPool` instances (`IThreadPool`) for modules that share parallel workers by name.
///
/// The logical name `"default"` is reserved for `defaultPool()` / `registerNamedPool("default", …)`.
///
/// Call `shutdownAll()` (or `unregister` each name) before process exit if you need joins to finish
/// deterministically; destroying a `ThreadPool` follows `ThreadPool`'s destructor contract.
class IThreadPoolRegistry {
public:
    virtual ~IThreadPoolRegistry() = default;

    [[nodiscard]] virtual std::shared_ptr<IThreadPool> registerNamedPool(std::string name,
        std::size_t threadCount = 0,
        std::size_t maxPendingSubmit = 0)
        = 0;

    [[nodiscard]] virtual std::shared_ptr<IThreadPool> get(std::string_view name) const = 0;
    [[nodiscard]] virtual std::vector<std::string> registeredNames() const = 0;
    virtual void unregister(std::string_view name) = 0;
    virtual void shutdownAll() = 0;

    [[nodiscard]] virtual std::shared_ptr<IThreadPool> defaultPool(std::size_t threadCount = 0,
        std::size_t maxPendingSubmit = 0)
        = 0;
};

class ThreadPoolRegistry final : public IThreadPoolRegistry {
public:
    explicit ThreadPoolRegistry(std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    ~ThreadPoolRegistry() override = default;

    ThreadPoolRegistry(const ThreadPoolRegistry&) = delete;
    ThreadPoolRegistry& operator=(const ThreadPoolRegistry&) = delete;
    ThreadPoolRegistry(ThreadPoolRegistry&&) = delete;
    ThreadPoolRegistry& operator=(ThreadPoolRegistry&&) = delete;

    /// Creates a `ThreadPool` for `name` if absent, otherwise returns the existing one.
    [[nodiscard]] std::shared_ptr<IThreadPool> registerNamedPool(std::string name,
        std::size_t threadCount = 0,
        std::size_t maxPendingSubmit = 0) override;

    /// Returns the named pool if registered, otherwise `nullptr`.
    [[nodiscard]] std::shared_ptr<IThreadPool> get(std::string_view name) const override;

    /// Snapshot of all registered names, sorted lexicographically (stable for logging / tests).
    [[nodiscard]] std::vector<std::string> registeredNames() const override;

    /// `shutdown(std::chrono::steady_clock::duration::max())` on the named pool (if present)
    /// and removes it from this registry.
    void unregister(std::string_view name) override;

    /// `shutdown(std::chrono::steady_clock::duration::max())` on every registered pool and clears the table.
    void shutdownAll() override;

    /// Same as `registerNamedPool("default", threadCount, maxPendingSubmit)` on this registry.
    [[nodiscard]] std::shared_ptr<IThreadPool> defaultPool(std::size_t threadCount = 0,
        std::size_t maxPendingSubmit = 0) override;

    [[nodiscard]] static constexpr std::string_view defaultPoolName() noexcept { return "default"; }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ThreadPool>> byName_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace lgc
