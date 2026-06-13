#include "foundation/threading/thread_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace lc {

std::shared_ptr<IThread> ThreadRegistry::registerNamed(std::string name, std::size_t maxPendingDispatch)
{
    if (name.empty()) {
        throw std::invalid_argument("ThreadRegistry::registerNamed: name must be non-empty");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = byName_.find(name);
    if (it != byName_.end()) {
        return it->second;
    }

    auto thread = std::make_shared<Thread>(name, maxPendingDispatch);
    const auto inserted = byName_.emplace(std::move(name), std::move(thread));
    return inserted.first->second;
}

std::shared_ptr<IThread> ThreadRegistry::get(std::string_view name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byName_.find(std::string(name));
    if (it == byName_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::string> ThreadRegistry::registeredNames() const
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

void ThreadRegistry::unregister(std::string_view name)
{
    std::shared_ptr<Thread> thread;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = byName_.find(std::string(name));
        if (it == byName_.end()) {
            return;
        }
        thread = it->second;
        byName_.erase(it);
    }
    if (thread) {
        (void)thread->shutdown(std::chrono::steady_clock::duration::max());
    }
}

void ThreadRegistry::shutdownAll()
{
    std::unordered_map<std::string, std::shared_ptr<Thread>> snapshot;
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

} // namespace lc
