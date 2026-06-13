#include "foundation/threading/thread_pool_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace lc {

std::shared_ptr<IThreadPool> ThreadPoolRegistry::registerNamedPool(std::string name,
    std::size_t threadCount,
    std::size_t maxPendingSubmit)
{
    if (name.empty()) {
        throw std::invalid_argument("ThreadPoolRegistry::registerNamedPool: name must be non-empty");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = byName_.find(name);
    if (it != byName_.end()) {
        return it->second;
    }

    auto pool = std::make_shared<ThreadPool>(threadCount, maxPendingSubmit);
    const auto inserted = byName_.emplace(std::move(name), std::move(pool));
    return inserted.first->second;
}

std::shared_ptr<IThreadPool> ThreadPoolRegistry::get(std::string_view name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byName_.find(std::string(name));
    if (it == byName_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::string> ThreadPoolRegistry::registeredNames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(byName_.size());
    for (const auto& entry : byName_) {
        out.push_back(entry.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ThreadPoolRegistry::unregister(std::string_view name)
{
    std::shared_ptr<ThreadPool> pool;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = byName_.find(std::string(name));
        if (it == byName_.end()) {
            return;
        }
        pool = it->second;
        byName_.erase(it);
    }
    if (pool) {
        (void)pool->shutdown(std::chrono::steady_clock::duration::max());
    }
}

void ThreadPoolRegistry::shutdownAll()
{
    std::unordered_map<std::string, std::shared_ptr<ThreadPool>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = std::move(byName_);
        byName_.clear();
    }
    for (auto& entry : snapshot) {
        if (entry.second) {
            (void)entry.second->shutdown(std::chrono::steady_clock::duration::max());
        }
    }
}

std::shared_ptr<IThreadPool> ThreadPoolRegistry::defaultPool(std::size_t threadCount,
    std::size_t maxPendingSubmit)
{
    return registerNamedPool(std::string(defaultPoolName()), threadCount, maxPendingSubmit);
}

} // namespace lc
