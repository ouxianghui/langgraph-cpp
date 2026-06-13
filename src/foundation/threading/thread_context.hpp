#pragma once

#include <spdlog/details/os.h>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace lc {

class ThreadContext final {
public:
    ThreadContext() = delete;

    static void setCurrentThreadName(std::string_view name)
    {
        currentName() = std::string(name);
        const auto tid = currentLogThreadId();
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            registry()[tid] = currentName();
        }
        setOsThreadName(currentName());
    }

    [[nodiscard]] static std::string currentThreadName()
    {
        return currentName();
    }

    [[nodiscard]] static std::string threadNameForLogThreadId(std::size_t tid)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        const auto it = registry().find(tid);
        if (it == registry().end()) {
            return {};
        }
        return it->second;
    }

    [[nodiscard]] static std::size_t currentLogThreadId()
    {
        return spdlog::details::os::thread_id();
    }

private:
    [[nodiscard]] static std::string& currentName()
    {
        thread_local std::string name;
        return name;
    }

    [[nodiscard]] static std::mutex& registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    [[nodiscard]] static std::unordered_map<std::size_t, std::string>& registry()
    {
        static std::unordered_map<std::size_t, std::string> names;
        return names;
    }

    static void setOsThreadName(const std::string& name) noexcept
    {
        if (name.empty()) {
            return;
        }
#if defined(__APPLE__)
        std::string truncated = name.substr(0, 63);
        (void)pthread_setname_np(truncated.c_str());
#elif defined(__linux__)
        std::string truncated = name.substr(0, 15);
        (void)pthread_setname_np(pthread_self(), truncated.c_str());
#else
        (void)name;
#endif
    }
};

} // namespace lc
