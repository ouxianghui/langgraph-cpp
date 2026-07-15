#pragma once

#include "foundation/event/i_event_sink.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace lgc {

struct QueuedEventSinkOptions {
    std::size_t capacity_ { 1024 };
    EventOverflowPolicy overflowPolicy_ { EventOverflowPolicy::Reject };
    EventSinkOptions event_;
};

class QueuedEventSink final : public IEventSink {
public:
    explicit QueuedEventSink(
        std::shared_ptr<IEventSink> inner,
        QueuedEventSinkOptions options = {});
    ~QueuedEventSink() override;

    QueuedEventSink(const QueuedEventSink&) = delete;
    QueuedEventSink& operator=(const QueuedEventSink&) = delete;
    QueuedEventSink(QueuedEventSink&&) = delete;
    QueuedEventSink& operator=(QueuedEventSink&&) = delete;

    [[nodiscard]] Status publish(RuntimeEvent event) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status waitIdle(Duration timeout) override;
    [[nodiscard]] Status close(Duration waitIdleTimeout) override;
    [[nodiscard]] bool isClosed() const noexcept override;

private:
    [[nodiscard]] Status waitIdleLocked(std::unique_lock<std::mutex>& lock, Duration timeout);
    [[nodiscard]] Status lastStatusLocked() const;
    void workerLoop();
    void recordWorkerStatus(const Status& status);

    std::shared_ptr<IEventSink> inner_;
    QueuedEventSinkOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::deque<RuntimeEvent> queue_;
    std::thread worker_;
    std::size_t inFlight_ { 0 };
    Status lastStatus_;
    bool closed_ { false };
};

} // namespace lgc
