#include "foundation/event/queued_event_sink.hpp"

#include "foundation/event/event_sink_utils.hpp"

#include <stdexcept>
#include <utility>

namespace lgc {
namespace {

[[nodiscard]] Status combineStatus(Status current, const Status& next)
{
    if (!current.isOk())
        return current;
    return next;
}

} // namespace

QueuedEventSink::QueuedEventSink(
    std::shared_ptr<IEventSink> inner,
    QueuedEventSinkOptions options)
    : inner_(std::move(inner))
    , options_(std::move(options))
{
    if (!inner_)
        throw std::invalid_argument("QueuedEventSink requires an inner sink");

    worker_ = std::thread([this] {
        workerLoop();
    });
}

QueuedEventSink::~QueuedEventSink()
{
    (void)close(std::chrono::seconds(5));
}

Status QueuedEventSink::publish(RuntimeEvent event)
{
    auto prepared = detail::prepareRuntimeEvent(std::move(event), options_.event_);
    if (!prepared.isOk())
        return prepared.status();

    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return Status::unavailable("event sink is closed");

        if (options_.capacity_ != 0U && queue_.size() >= options_.capacity_) {
            switch (options_.overflowPolicy_) {
            case EventOverflowPolicy::Reject:
                return Status::resourceExhausted("queued event sink is full");
            case EventOverflowPolicy::DropOldest:
                queue_.pop_front();
                break;
            case EventOverflowPolicy::DropNewest:
                return Status::ok();
            }
        }

        queue_.push_back(std::move(*prepared));
    }

    cv_.notify_one();
    return Status::ok();
}

Status QueuedEventSink::flush()
{
    auto status = waitIdle(Duration::max());
    status = combineStatus(std::move(status), inner_->flush());

    std::lock_guard lock(mutex_);
    return combineStatus(std::move(status), lastStatusLocked());
}

Status QueuedEventSink::waitIdle(Duration timeout)
{
    std::unique_lock lock(mutex_);
    auto status = waitIdleLocked(lock, timeout);
    return combineStatus(std::move(status), lastStatusLocked());
}

Status QueuedEventSink::close(Duration waitIdleTimeout)
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();

    auto status = waitIdle(waitIdleTimeout);
    if (!status.isOk()) {
        std::lock_guard lock(mutex_);
        queue_.clear();
        if (inFlight_ == 0)
            idleCv_.notify_all();
    }

    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
        worker_.join();

    status = combineStatus(std::move(status), inner_->flush());
    status = combineStatus(std::move(status), inner_->close(waitIdleTimeout));

    std::lock_guard lock(mutex_);
    return combineStatus(std::move(status), lastStatusLocked());
}

bool QueuedEventSink::isClosed() const noexcept
{
    std::lock_guard lock(mutex_);
    return closed_;
}

Status QueuedEventSink::waitIdleLocked(std::unique_lock<std::mutex>& lock, Duration timeout)
{
    const auto idle = [this] {
        return queue_.empty() && inFlight_ == 0;
    };

    if (idle())
        return Status::ok();

    if (timeout <= Duration::zero())
        return Status::deadlineExceeded("event sink did not become idle before timeout");

    if (timeout == Duration::max()) {
        idleCv_.wait(lock, idle);
        return Status::ok();
    }

    if (!idleCv_.wait_for(lock, timeout, idle))
        return Status::deadlineExceeded("event sink did not become idle before timeout");

    return Status::ok();
}

Status QueuedEventSink::lastStatusLocked() const
{
    return lastStatus_;
}

void QueuedEventSink::workerLoop()
{
    while (true) {
        RuntimeEvent event;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return closed_ || !queue_.empty();
            });

            if (queue_.empty()) {
                if (closed_)
                    return;
                continue;
            }

            event = std::move(queue_.front());
            queue_.pop_front();
            ++inFlight_;
        }

        auto status = inner_->publish(std::move(event));
        if (!status.isOk())
            recordWorkerStatus(status);

        {
            std::lock_guard lock(mutex_);
            if (inFlight_ > 0)
                --inFlight_;
            if (queue_.empty() && inFlight_ == 0)
                idleCv_.notify_all();
        }
    }
}

void QueuedEventSink::recordWorkerStatus(const Status& status)
{
    std::lock_guard lock(mutex_);
    if (lastStatus_.isOk())
        lastStatus_ = status;
}

} // namespace lgc
