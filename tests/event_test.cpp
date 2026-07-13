#include "foundation/event/callback_event_sink.hpp"
#include "foundation/event/memory_event_sink.hpp"
#include "foundation/event/queued_event_sink.hpp"
#include "foundation/event/runtime_event.hpp"
#include "foundation/status/status.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

int main()
{
    using nlohmann::json;
    using namespace std::chrono_literals;

    auto event = lc::RuntimeEvent::create(lc::RuntimeEventType::NodeStarted);
    event.runId_ = "run-1";
    event.threadId_ = "thread-1";
    event.step_ = 3;
    event.sequence_ = 7;
    event.node_ = "planner";
    event.payload_ = json {
        { "input_tokens", 12 },
    };

    assert(lc::runtimeEventTypeName(event.type_) == "node_started");
    assert(!event.eventId_.empty());
    assert(event.sequence_ > 0);
    assert(lc::validateRuntimeEvent(event).isOk());

    lc::MemoryEventSink memorySink;
    auto status = memorySink.publish(event);
    assert(status.isOk());
    assert(memorySink.size() == 1);
    auto snapshot = memorySink.events();
    assert(snapshot.size() == 1);
    assert(snapshot.front().node_ == "planner");
    assert(snapshot.front().payload_.at("input_tokens") == 12);

    memorySink.clear();
    assert(memorySink.size() == 0);
    assert(memorySink.flush().isOk());
    assert(memorySink.waitIdle(0ms).isOk());
    assert(memorySink.close(100ms).isOk());
    assert(memorySink.isClosed());
    status = memorySink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::Unavailable);

    lc::MemoryEventSink boundedSink(lc::MemoryEventSinkOptions {
        .capacity_ = 1,
        .overflowPolicy_ = lc::EventOverflowPolicy::Reject,
    });
    assert(boundedSink.publish(event).isOk());
    status = boundedSink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    lc::MemoryEventSink identitySink;
    lc::RuntimeEvent manual;
    manual.type_ = lc::RuntimeEventType::RunStarted;
    assert(identitySink.publish(std::move(manual)).isOk());
    auto identityEvents = identitySink.events();
    assert(identityEvents.size() == 1);
    assert(!identityEvents.front().eventId_.empty());
    assert(identityEvents.front().sequence_ > 0);

    auto first = event;
    first.sequence_ = 1;
    auto second = event;
    second.sequence_ = 2;

    lc::MemoryEventSink dropOldestSink(lc::MemoryEventSinkOptions {
        .capacity_ = 1,
        .overflowPolicy_ = lc::EventOverflowPolicy::DropOldest,
    });
    assert(dropOldestSink.publish(first).isOk());
    assert(dropOldestSink.publish(second).isOk());
    assert(dropOldestSink.events().size() == 1);
    assert(dropOldestSink.events().front().sequence_ == 2);

    lc::MemoryEventSink dropNewestSink(lc::MemoryEventSinkOptions {
        .capacity_ = 1,
        .overflowPolicy_ = lc::EventOverflowPolicy::DropNewest,
    });
    assert(dropNewestSink.publish(first).isOk());
    assert(dropNewestSink.publish(second).isOk());
    assert(dropNewestSink.events().size() == 1);
    assert(dropNewestSink.events().front().sequence_ == 1);

    auto invalid = lc::RuntimeEvent::create(lc::RuntimeEventType::Unknown);
    status = boundedSink.publish(invalid);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::InvalidArgument);

    auto missingSequence = event;
    missingSequence.sequence_ = 0;
    status = lc::validateRuntimeEvent(missingSequence);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::InvalidArgument);

    auto badEventId = event;
    badEventId.eventId_ = "bad id";
    status = lc::validateRuntimeEvent(badEventId);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::InvalidArgument);

    auto custom = lc::RuntimeEvent::create(lc::RuntimeEventType::Custom);
    status = lc::validateRuntimeEvent(custom);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::InvalidArgument);
    custom.name_ = "debug.note";
    assert(lc::validateRuntimeEvent(custom).isOk());

    auto badRunId = event;
    badRunId.runId_ = "bad id with spaces";
    status = lc::validateRuntimeEvent(badRunId);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::InvalidArgument);

    auto tooLarge = event;
    tooLarge.payload_ = json {
        { "text", std::string(64, 'x') },
    };
    status = lc::validateRuntimeEvent(tooLarge, lc::RuntimeEventLimits {
                                                   .maxPayloadBytes_ = 16,
                                               });
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    auto tooDeep = event;
    tooDeep.payload_ = json {
        { "a", {
                   { "b", {
                              { "c", 1 },
                          } },
               } },
    };
    status = lc::validateRuntimeEvent(tooDeep, lc::RuntimeEventLimits {
                                                 .maxJsonDepth_ = 2,
                                             });
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    auto tooManyNodes = event;
    tooManyNodes.payload_ = json::array({ 1, 2, 3 });
    status = lc::validateRuntimeEvent(tooManyNodes, lc::RuntimeEventLimits {
                                                        .maxJsonNodes_ = 2,
                                                    });
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    auto tooManyItems = event;
    tooManyItems.payload_ = json::array({ 1, 2, 3 });
    status = lc::validateRuntimeEvent(tooManyItems, lc::RuntimeEventLimits {
                                                        .maxJsonItems_ = 2,
                                                    });
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    auto tooLong = lc::RuntimeEvent::create(
        lc::RuntimeEventType::NodeStarted,
        lc::RuntimeEventOptions {
            .generateEventId_ = false,
        });
    tooLong.eventId_ = "id";
    tooLong.message_ = std::string(13, 'x');
    status = lc::validateRuntimeEvent(tooLong, lc::RuntimeEventLimits {
                                                 .maxStringLength_ = 12,
                                             });
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    auto tooLongPayloadString = event;
    tooLongPayloadString.payload_ = json {
        { "text", std::string(13, 'x') },
    };
    status = lc::validateRuntimeEvent(tooLongPayloadString, lc::RuntimeEventLimits {
                                                                .maxStringLength_ = 12,
                                                            });
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::ResourceExhausted);

    lc::MemoryEventSink redactingSink;
    auto sensitive = lc::RuntimeEvent::create(lc::RuntimeEventType::ToolCallStarted);
    sensitive.message_ = "Authorization: Bearer abcdefghijklmnop for user@example.com";
    sensitive.payload_ = json {
        { "api_key", "sk-1234567890abcdef" },
        { "input", "normal" },
    };
    assert(redactingSink.publish(std::move(sensitive)).isOk());
    auto redacted = redactingSink.events();
    assert(redacted.size() == 1);
    assert(redacted.front().message_.find("abcdefghijklmnop") == std::string::npos);
    assert(redacted.front().message_.find("user@example.com") == std::string::npos);
    assert(redacted.front().payload_["api_key"] == "[REDACTED]");
    assert(redacted.front().payload_["input"] == "normal");

    std::vector<lc::RuntimeEvent> observed;
    lc::CallbackEventSink callbackSink(lc::CallbackEventSink::VoidCallback(
        [&observed](const lc::RuntimeEvent& item) {
            observed.push_back(item);
        }));
    assert(callbackSink.publish(event).isOk());
    assert(observed.size() == 1);
    assert(observed.front().type_ == lc::RuntimeEventType::NodeStarted);

    lc::CallbackEventSink statusCallbackSink(lc::CallbackEventSink::Callback(
        [](const lc::RuntimeEvent&) {
            return lc::Status::permissionDenied("observer denied");
        }));
    status = statusCallbackSink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::PermissionDenied);

    lc::CallbackEventSink throwingSink(lc::CallbackEventSink::VoidCallback(
        [](const lc::RuntimeEvent&) {
            throw std::runtime_error("boom");
        }));
    status = throwingSink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::Internal);

    {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool release = false;
        lc::CallbackEventSink blockingSink(lc::CallbackEventSink::VoidCallback(
            [&](const lc::RuntimeEvent&) {
                std::unique_lock lock(mutex);
                started = true;
                cv.notify_all();
                cv.wait(lock, [&] {
                    return release;
                });
            }));

        std::thread worker([&] {
            assert(blockingSink.publish(event).isOk());
        });

        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] {
                return started;
            });
        }
        status = blockingSink.waitIdle(1ms);
        assert(!status.isOk());
        assert(status.code() == lc::StatusCode::DeadlineExceeded);
        status = blockingSink.close(1ms);
        assert(!status.isOk());
        assert(status.code() == lc::StatusCode::DeadlineExceeded);
        assert(blockingSink.isClosed());

        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        worker.join();
        assert(blockingSink.waitIdle(1s).isOk());
        assert(blockingSink.close(1s).isOk());
    }

    {
        lc::MemoryEventSink concurrentSink(lc::MemoryEventSinkOptions {
            .capacity_ = 1000,
        });
        std::atomic<int> failures { 0 };
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&concurrentSink, &event, &failures, worker] {
                for (int i = 0; i < 50; ++i) {
                    auto item = event;
                    item.sequence_ = static_cast<std::uint64_t>(worker * 1000 + i + 1);
                    if (!concurrentSink.publish(std::move(item)).isOk())
                        ++failures;
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(failures.load() == 0);
        assert(concurrentSink.size() == 200);
    }

    assert(callbackSink.close(100ms).isOk());
    assert(callbackSink.isClosed());
    status = callbackSink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lc::StatusCode::Unavailable);

    {
        auto inner = std::make_shared<lc::MemoryEventSink>();
        lc::QueuedEventSink queued(inner, lc::QueuedEventSinkOptions {
                                              .capacity_ = 4,
                                          });

        auto queuedEvent = event;
        queuedEvent.sequence_ = 42;
        assert(queued.publish(queuedEvent).isOk());
        assert(queued.waitIdle(1s).isOk());
        assert(inner->size() == 1);
        assert(inner->events().front().sequence_ == 42);
        assert(queued.flush().isOk());
        assert(queued.close(1s).isOk());
        assert(queued.isClosed());
        status = queued.publish(queuedEvent);
        assert(!status.isOk());
        assert(status.code() == lc::StatusCode::Unavailable);
    }

    {
        std::mutex mutex;
        std::condition_variable cv;
        bool callbackStarted = false;
        bool release = false;
        auto inner = std::make_shared<lc::CallbackEventSink>(lc::CallbackEventSink::VoidCallback(
            [&](const lc::RuntimeEvent&) {
                std::unique_lock lock(mutex);
                callbackStarted = true;
                cv.notify_all();
                cv.wait(lock, [&] {
                    return release;
                });
            }));
        lc::QueuedEventSink queued(inner, lc::QueuedEventSinkOptions {
                                              .capacity_ = 4,
                                          });

        assert(queued.publish(event).isOk());
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, 1s, [&] {
                return callbackStarted;
            });
            assert(callbackStarted);
        }
        assert(queued.publish(event).isOk());
        status = queued.waitIdle(1ms);
        assert(!status.isOk());
        assert(status.code() == lc::StatusCode::DeadlineExceeded);

        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        assert(queued.close(1s).isOk());
    }

    return 0;
}
