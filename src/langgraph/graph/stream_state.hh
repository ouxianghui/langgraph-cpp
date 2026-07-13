#pragma once

#include "foundation/async/channel.hpp"
#include "foundation/status/result.hpp"
#include "langgraph/graph/stream.hpp"

#include <future>
#include <thread>

namespace lc {

struct RunEventStreamState final {
    explicit RunEventStreamState(std::size_t capacity);
    ~RunEventStreamState();

    RunEventStreamState(const RunEventStreamState&) = delete;
    RunEventStreamState& operator=(const RunEventStreamState&) = delete;

    void close() noexcept;

    BoundedChannel<RuntimeEvent> channel_;
    std::promise<Result<RunResult>> resultPromise_;
    std::shared_future<Result<RunResult>> resultFuture_;
    std::thread worker_;
};

struct RunPartStreamState final {
    explicit RunPartStreamState(std::size_t capacity);
    ~RunPartStreamState();

    RunPartStreamState(const RunPartStreamState&) = delete;
    RunPartStreamState& operator=(const RunPartStreamState&) = delete;

    void close() noexcept;

    BoundedChannel<StreamPart> channel_;
    std::promise<Result<RunResult>> resultPromise_;
    std::shared_future<Result<RunResult>> resultFuture_;
    std::thread worker_;
};

} // namespace lc
