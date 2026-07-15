#pragma once

#include "foundation/executor/i_executor.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lgc {

[[nodiscard]] inline std::shared_ptr<IExecutor> requireExecutor(
    std::shared_ptr<IExecutor> executor,
    std::string_view owner,
    std::string_view name)
{
    if (!executor) {
        throw std::invalid_argument(std::string(owner) + ": required executor is not configured: " + std::string(name));
    }
    return executor;
}

inline void requireOnExecutor(
    const std::shared_ptr<IExecutor>& executor,
    std::string_view owner,
    std::string_view name)
{
    (void)requireExecutor(executor, owner, name);
    if (!executor->isExecutorThread()) {
        throw std::logic_error(std::string(owner) + ": must run on executor: " + std::string(name));
    }
}

} // namespace lgc
