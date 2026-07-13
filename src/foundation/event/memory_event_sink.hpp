#pragma once

#include "foundation/event/i_event_sink.hpp"

#include <cstddef>
#include <mutex>
#include <vector>

namespace lc {

struct MemoryEventSinkOptions {
    std::size_t capacity_ { 1024 };
    EventOverflowPolicy overflowPolicy_ { EventOverflowPolicy::Reject };
    EventSinkOptions event_;
};

class MemoryEventSink final : public IEventSink {
public:
    /// `capacity_ == 0` means unbounded.
    explicit MemoryEventSink(MemoryEventSinkOptions options = {});
    ~MemoryEventSink() override = default;

    MemoryEventSink(const MemoryEventSink&) = delete;
    MemoryEventSink& operator=(const MemoryEventSink&) = delete;
    MemoryEventSink(MemoryEventSink&&) = delete;
    MemoryEventSink& operator=(MemoryEventSink&&) = delete;

    [[nodiscard]] Status publish(RuntimeEvent event) override;
    [[nodiscard]] Status flush() override;
    [[nodiscard]] Status waitIdle(Duration timeout) override;
    [[nodiscard]] Status close(Duration waitIdleTimeout) override;
    [[nodiscard]] bool isClosed() const noexcept override;

    [[nodiscard]] std::vector<RuntimeEvent> events() const;
    [[nodiscard]] std::size_t size() const noexcept;
    void clear();

private:
    MemoryEventSinkOptions options_;
    mutable std::mutex mutex_;
    std::vector<RuntimeEvent> events_;
    bool closed_ { false };
};

} // namespace lc
