#pragma once

#include "foundation/threading/thread.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lgc {

class IThreadRegistry {
public:
    virtual ~IThreadRegistry() = default;

    [[nodiscard]] virtual std::shared_ptr<IThread> registerNamed(std::string name,
        std::size_t maxPendingDispatch = 0)
        = 0;
    [[nodiscard]] virtual std::shared_ptr<IThread> get(std::string_view name) const = 0;
    [[nodiscard]] virtual std::vector<std::string> registeredNames() const = 0;
    virtual void unregister(std::string_view name) = 0;
    virtual void shutdownAll() = 0;
};

/// Owns named `Thread` instances (`IThread`) for modules that share serial dispatchers by logical name.
///
/// For named parallel pools see `ThreadPoolRegistry` (`IThreadPool` / `ThreadPool`).
///
/// C++ reserves the identifier `register`; use `registerNamed(std::move(name))` for the intuitive
/// "get or create by name" operation.
///
/// Call `shutdownAll()` (or `unregister` each name) before process exit if you need workers joined
/// deterministically; destroying a `Thread` still follows `Thread`'s destructor contract.
///
class ThreadRegistry final : public IThreadRegistry {
public:
    explicit ThreadRegistry(std::shared_ptr<ILogger> logger = Logger::defaultLogger());
    ~ThreadRegistry() override = default;

    ThreadRegistry(const ThreadRegistry&) = delete;
    ThreadRegistry& operator=(const ThreadRegistry&) = delete;
    ThreadRegistry(ThreadRegistry&&) = delete;
    ThreadRegistry& operator=(ThreadRegistry&&) = delete;

    /// Creates a `Thread` for `name` if absent, otherwise returns the existing one.
    [[nodiscard]] std::shared_ptr<IThread> registerNamed(std::string name,
        std::size_t maxPendingDispatch = 0) override;

    /// Returns the named thread if registered, otherwise `nullptr`.
    [[nodiscard]] std::shared_ptr<IThread> get(std::string_view name) const override;

    /// Snapshot of all registered names, sorted lexicographically (stable for logging / tests).
    [[nodiscard]] std::vector<std::string> registeredNames() const override;

    /// `shutdown(std::chrono::steady_clock::duration::max())` on the named thread (if present)
    /// and removes it from this registry.
    void unregister(std::string_view name) override;

    /// `shutdown(std::chrono::steady_clock::duration::max())` on every registered thread and clears the table.
    void shutdownAll() override;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Thread>> byName_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace lgc
