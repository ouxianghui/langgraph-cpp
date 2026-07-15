#pragma once

#include "foundation/threading/i_thread.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lgc {

[[nodiscard]] inline std::shared_ptr<IThread> requireThread(
    std::shared_ptr<IThread> thread,
    std::string_view owner,
    std::string_view name)
{
    if (!thread) {
        throw std::invalid_argument(std::string(owner) + ": required thread is not configured: " + std::string(name));
    }
    return thread;
}

inline void requireOnThread(
    const std::shared_ptr<IThread>& thread,
    std::string_view owner,
    std::string_view name)
{
    (void)requireThread(thread, owner, name);
    if (!thread->isCurrentThread()) {
        throw std::logic_error(std::string(owner) + ": must run on thread: " + std::string(name));
    }
}

} // namespace lgc
