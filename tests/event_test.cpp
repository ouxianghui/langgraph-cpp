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

    auto event = lgc::RuntimeEvent::create(lgc::RuntimeEventType::NodeStarted);
    event.runId_ = "run-1";
    event.threadId_ = "thread-1";
    event.step_ = 3;
    event.sequence_ = 7;
    event.node_ = "planner";
    event.payload_ = json {
        { "input_tokens", 12 },
    };

    assert(lgc::runtimeEventTypeName(event.type_) == "node_started");
    assert(!event.eventId_.empty());
    assert(event.sequence_ > 0);
    assert(lgc::validateRuntimeEvent(event).isOk());

    lgc::MemoryEventSink memorySink;
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
    assert(status.code() == lgc::StatusCode::Unavailable);

    lgc::MemoryEventSink boundedSink(lgc::MemoryEventSinkOptions {
        .capacity_ = 1,
        .overflowPolicy_ = lgc::EventOverflowPolicy::Reject,
    });
    assert(boundedSink.publish(event).isOk());
    status = boundedSink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::ResourceExhausted);

    lgc::MemoryEventSink identitySink;
    lgc::RuntimeEvent manual;
    manual.type_ = lgc::RuntimeEventType::RunStarted;
    assert(identitySink.publish(std::move(manual)).isOk());
    auto identityEvents = identitySink.events();
    assert(identityEvents.size() == 1);
    assert(!identityEvents.front().eventId_.empty());
    assert(identityEvents.front().sequence_ > 0);

    auto first = event;
    first.sequence_ = 1;
    auto second = event;
    second.sequence_ = 2;

    lgc::MemoryEventSink dropOldestSink(lgc::MemoryEventSinkOptions {
        .capacity_ = 1,
        .overflowPolicy_ = lgc::EventOverflowPolicy::DropOldest,
    });
    assert(dropOldestSink.publish(first).isOk());
    assert(dropOldestSink.publish(second).isOk());
    assert(dropOldestSink.events().size() == 1);
    assert(dropOldestSink.events().front().sequence_ == 2);

    lgc::MemoryEventSink dropNewestSink(lgc::MemoryEventSinkOptions {
        .capacity_ = 1,
        .overflowPolicy_ = lgc::EventOverflowPolicy::DropNewest,
    });
    assert(dropNewestSink.publish(first).isOk());
    assert(dropNewestSink.publish(second).isOk());
    assert(dropNewestSink.events().size() == 1);
    assert(dropNewestSink.events().front().sequence_ == 1);

    auto invalid = lgc::RuntimeEvent::create(lgc::RuntimeEventType::Unknown);
    status = boundedSink.publish(invalid);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::InvalidArgument);

    auto missingSequence = event;
    missingSequence.sequence_ = 0;
    status = lgc::validateRuntimeEvent(missingSequence);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::InvalidArgument);

    auto badEventId = event;
    badEventId.eventId_ = "bad id";
    status = lgc::validateRuntimeEvent(badEventId);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::InvalidArgument);

    auto custom = lgc::RuntimeEvent::create(lgc::RuntimeEventType::Custom);
    status = lgc::validateRuntimeEvent(custom);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::InvalidArgument);
    custom.name_ = "debug.note";
    assert(lgc::validateRuntimeEvent(custom).isOk());

    auto badRunId = event;
    badRunId.runId_ = "bad id with spaces";
    status = lgc::validateRuntimeEvent(badRunId);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::InvalidArgument);

    auto tooLarge = event;
    tooLarge.payload_ = json {
        { "text", std::string(64, 'x') },
    };
    status = lgc::validateRuntimeEvent(tooLarge, lgc::RuntimeEventLimits {
                                                   .maxPayloadBytes_ = 16,
                                               });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::ResourceExhausted);

    auto tooDeep = event;
    tooDeep.payload_ = json {
        { "a", {
                   { "b", {
                              { "c", 1 },
                          } },
               } },
    };
    status = lgc::validateRuntimeEvent(tooDeep, lgc::RuntimeEventLimits {
                                                 .maxJsonDepth_ = 2,
                                             });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::ResourceExhausted);

    auto tooManyNodes = event;
    tooManyNodes.payload_ = json::array({ 1, 2, 3 });
    status = lgc::validateRuntimeEvent(tooManyNodes, lgc::RuntimeEventLimits {
                                                        .maxJsonNodes_ = 2,
                                                    });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::ResourceExhausted);

    auto tooManyItems = event;
    tooManyItems.payload_ = json::array({ 1, 2, 3 });
    status = lgc::validateRuntimeEvent(tooManyItems, lgc::RuntimeEventLimits {
                                                        .maxJsonItems_ = 2,
                                                    });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::ResourceExhausted);

    auto tooLong = lgc::RuntimeEvent::create(
        lgc::RuntimeEventType::NodeStarted,
        lgc::RuntimeEventOptions {
            .generateEventId_ = false,
        });
    tooLong.eventId_ = "id";
    tooLong.message_ = std::string(13, 'x');
    status = lgc::validateRuntimeEvent(tooLong, lgc::RuntimeEventLimits {
                                                 .maxStringLength_ = 12,
                                             });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::ResourceExhausted);

    auto tooLongPayloadString = event;
    tooLongPayloadString.payload_ = json {
        { "text", std::string(13, 'x') },
    };
    status = lgc::validateRuntimeEvent(tooLongPayloadString, lgc::RuntimeEventLimits {
                                                                .maxStringLength_ = 12,
                                                            });
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::ResourceExhausted);

    lgc::MemoryEventSink redactingSink;
    auto sensitive = lgc::RuntimeEvent::create(lgc::RuntimeEventType::ToolCallStarted);
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

    std::vector<lgc::RuntimeEvent> observed;
    lgc::CallbackEventSink callbackSink(lgc::CallbackEventSink::VoidCallback(
        [&observed](const lgc::RuntimeEvent& item) {
            observed.push_back(item);
        }));
    assert(callbackSink.publish(event).isOk());
    assert(observed.size() == 1);
    assert(observed.front().type_ == lgc::RuntimeEventType::NodeStarted);

    lgc::CallbackEventSink statusCallbackSink(lgc::CallbackEventSink::Callback(
        [](const lgc::RuntimeEvent&) {
            return lgc::Status::permissionDenied("observer denied");
        }));
    status = statusCallbackSink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::PermissionDenied);

    lgc::CallbackEventSink throwingSink(lgc::CallbackEventSink::VoidCallback(
        [](const lgc::RuntimeEvent&) {
            throw std::runtime_error("boom");
        }));
    status = throwingSink.publish(event);
    assert(!status.isOk());
    assert(status.code() == lgc::StatusCode::Internal);

    {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool release = false;
        lgc::CallbackEventSink blockingSink(lgc::CallbackEventSink::VoidCallback(
            [&](const lgc::RuntimeEvent&) {
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
        assert(status.code() == lgc::StatusCode::DeadlineExceeded);
        status = blockingSink.close(1ms);
        assert(!status.isOk());
        assert(status.code() == lgc::StatusCode::DeadlineExceeded);
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
        lgc::MemoryEventSink concurrentSink(lgc::MemoryEventSinkOptions {
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
    assert(status.code() == lgc::StatusCode::Unavailable);

    {
        auto inner = std::make_shared<lgc::MemoryEventSink>();
        lgc::QueuedEventSink queued(inner, lgc::QueuedEventSinkOptions {
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
        assert(status.code() == lgc::StatusCode::Unavailable);
    }

    {
        std::mutex mutex;
        std::condition_variable cv;
        bool callbackStarted = false;
        bool release = false;
        auto inner = std::make_shared<lgc::CallbackEventSink>(lgc::CallbackEventSink::VoidCallback(
            [&](const lgc::RuntimeEvent&) {
                std::unique_lock lock(mutex);
                callbackStarted = true;
                cv.notify_all();
                cv.wait(lock, [&] {
                    return release;
                });
            }));
        lgc::QueuedEventSink queued(inner, lgc::QueuedEventSinkOptions {
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
        assert(status.code() == lgc::StatusCode::DeadlineExceeded);

        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        assert(queued.close(1s).isOk());
    }

    return 0;
}
