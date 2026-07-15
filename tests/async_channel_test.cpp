#include "foundation/async/channel.hpp"
#include "foundation/async/future.hpp"
#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <atomic>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

int main()
{
    using namespace std::chrono_literals;

    bool threwCapacity = false;
    try {
        lgc::BoundedChannel<int> invalid(0);
    } catch (const std::invalid_argument&) {
        threwCapacity = true;
    }
    assert(threwCapacity);

    lgc::Promise<int> promise;
    auto future = promise.future();
    assert(future.valid());
    assert(!future.ready());
    assert(!future.wait(0ms));

    std::thread producer([p = std::move(promise)]() mutable {
        std::this_thread::sleep_for(10ms);
        assert(p.resolve(42).isOk());
    });
    assert(future.wait(1s));
    assert(future.get().value() == 42);
    assert(future.take().value() == 42);
    assert(future.take().status().code() == lgc::StatusCode::FailedPrecondition);
    producer.join();

    lgc::Promise<std::string> errorPromise;
    auto errorFuture = errorPromise.future();
    assert(errorPromise.reject(lgc::Status::unavailable("worker stopped")).isOk());
    auto error = errorFuture.get();
    assert(!error.isOk());
    assert(error.status().code() == lgc::StatusCode::Unavailable);

    lgc::Future<int> abandonedFuture;
    {
        lgc::Promise<int> abandoned;
        abandonedFuture = abandoned.future();
    }
    auto abandoned = abandonedFuture.get();
    assert(!abandoned.isOk());
    assert(abandoned.status().code() == lgc::StatusCode::Cancelled);

    lgc::Promise<void> voidPromise;
    auto voidFuture = voidPromise.future();
    assert(voidPromise.resolve().isOk());
    assert(voidFuture.get().isOk());

    lgc::Promise<std::unique_ptr<int>> moveOnlyPromise;
    auto moveOnlyFuture = moveOnlyPromise.future();
    assert(moveOnlyPromise.resolve(std::make_unique<int>(7)).isOk());
    auto moved = moveOnlyFuture.take();
    assert(moved.isOk());
    assert(**moved == 7);

    lgc::BoundedChannel<std::string> channel(2);
    assert(channel.capacity() == 2);
    assert(channel.trySend("one").isOk());
    assert(channel.trySend("two").isOk());
    auto full = channel.trySend("three");
    assert(!full.isOk());
    assert(full.code() == lgc::StatusCode::ResourceExhausted);

    auto first = channel.receive();
    assert(first.isOk());
    assert(first->has_value());
    assert(first->value() == "one");

    std::thread sender([&channel] {
        assert(channel.send("three").isOk());
    });
    sender.join();

    auto second = channel.receive();
    auto third = channel.receive();
    assert(second.isOk());
    assert(second->has_value());
    assert(second->value() == "two");
    assert(third.isOk());
    assert(third->has_value());
    assert(third->value() == "three");
    assert(channel.receiveFor(1ms).status().code() == lgc::StatusCode::DeadlineExceeded);

    std::thread blockingReceiver([&channel] {
        auto item = channel.receive();
        assert(item.isOk());
        assert(item->has_value());
        assert(item->value() == "async");
    });
    std::this_thread::sleep_for(10ms);
    assert(channel.send("async").isOk());
    blockingReceiver.join();

    assert(channel.tryReceive().status().code() == lgc::StatusCode::Unavailable);
    channel.close();
    assert(channel.isClosed());
    auto drained = channel.receive();
    assert(drained.isOk());
    assert(!drained->has_value());
    assert(channel.trySend("closed").code() == lgc::StatusCode::Unavailable);

    auto stats = channel.stats();
    assert(stats.closed_);
    assert(stats.sent_ == 4);
    assert(stats.received_ == 4);
    assert(stats.rejectedAfterClose_ == 1);

    lgc::BoundedChannel<int> endpointChannel(1);
    auto endpointSender = endpointChannel.sender();
    auto endpointReceiver = endpointChannel.receiver();
    assert(endpointSender.valid());
    assert(endpointReceiver.valid());
    assert(endpointSender.send(11).isOk());
    auto endpointValue = endpointReceiver.receive();
    assert(endpointValue.isOk());
    assert(endpointValue->has_value());
    assert(**endpointValue == 11);

    lgc::BoundedChannel<int> blockingChannel(1);
    assert(blockingChannel.trySend(1).isOk());
    auto sendTimeout = blockingChannel.sendFor(2, 1ms);
    assert(!sendTimeout.isOk());
    assert(sendTimeout.code() == lgc::StatusCode::DeadlineExceeded);

    lgc::BoundedChannel<int> concurrentChannel(4);
    std::atomic<int> receivedSum { 0 };
    std::thread producerA([&] {
        for (int i = 1; i <= 10; ++i)
            assert(concurrentChannel.send(i).isOk());
    });
    std::thread producerB([&] {
        for (int i = 11; i <= 20; ++i)
            assert(concurrentChannel.send(i).isOk());
    });
    std::thread consumerA([&] {
        for (int i = 0; i < 10; ++i) {
            auto item = concurrentChannel.receive();
            assert(item.isOk());
            assert(item->has_value());
            receivedSum.fetch_add(**item, std::memory_order_relaxed);
        }
    });
    std::thread consumerB([&] {
        for (int i = 0; i < 10; ++i) {
            auto item = concurrentChannel.receive();
            assert(item.isOk());
            assert(item->has_value());
            receivedSum.fetch_add(**item, std::memory_order_relaxed);
        }
    });
    producerA.join();
    producerB.join();
    consumerA.join();
    consumerB.join();
    assert(receivedSum.load(std::memory_order_relaxed) == 210);

    lgc::UnbufferedChannel<std::string> rendezvous;
    assert(rendezvous.trySend("no receiver").code() == lgc::StatusCode::ResourceExhausted);
    std::atomic<bool> sendCompleted { false };
    std::thread rendezvousSender([&] {
        assert(rendezvous.send("handoff").isOk());
        sendCompleted.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(10ms);
    assert(!sendCompleted.load(std::memory_order_acquire));
    auto rendezvousValue = rendezvous.receive();
    assert(rendezvousValue.isOk());
    assert(rendezvousValue->has_value());
    assert(rendezvousValue->value() == "handoff");
    rendezvousSender.join();
    assert(sendCompleted.load(std::memory_order_acquire));

    lgc::CancellationSource receiveCancel;
    lgc::BoundedChannel<int> cancellableChannel(1);
    std::thread canceller([&] {
        std::this_thread::sleep_for(10ms);
        assert(receiveCancel.cancel("stop receive"));
    });
    auto cancelledReceive = cancellableChannel.receive(receiveCancel.token());
    assert(!cancelledReceive.isOk());
    assert(cancelledReceive.status().code() == lgc::StatusCode::Cancelled);
    canceller.join();

    lgc::CancellationSource sendCancel;
    lgc::BoundedChannel<int> fullChannel(1);
    assert(fullChannel.send(1).isOk());
    std::thread sendCanceller([&] {
        std::this_thread::sleep_for(10ms);
        assert(sendCancel.cancel("stop send"));
    });
    auto cancelledSend = fullChannel.send(2, sendCancel.token());
    assert(!cancelledSend.isOk());
    assert(cancelledSend.code() == lgc::StatusCode::Cancelled);
    sendCanceller.join();

    lgc::BoundedChannel<int> selectA(1);
    lgc::BoundedChannel<int> selectB(1);
    auto selectAReceiver = selectA.receiver();
    auto selectBReceiver = selectB.receiver();
    std::thread selectProducer([&] {
        std::this_thread::sleep_for(10ms);
        assert(selectB.send(99).isOk());
    });
    int selectedValue = 0;
    bool selectedClosed = false;
    auto selected = lgc::select(
        lgc::recv(selectAReceiver, [&](lgc::ChannelReceive<int> received) {
            selectedClosed = received.closed_;
            if (received.value_.has_value())
                selectedValue = *received.value_;
        }),
        lgc::recv(selectBReceiver, [&](lgc::ChannelReceive<int> received) {
            selectedClosed = received.closed_;
            if (received.value_.has_value())
                selectedValue = *received.value_;
        }));
    assert(selected.isOk());
    assert(*selected == 1);
    assert(selectedValue == 99);
    assert(!selectedClosed);
    selectProducer.join();

    bool timeoutSelected = false;
    auto selectTimeout = lgc::select(
        lgc::recv(selectAReceiver, [](lgc::ChannelReceive<int>) {
            assert(false);
        }),
        lgc::recv(selectBReceiver, [](lgc::ChannelReceive<int>) {
            assert(false);
        }),
        lgc::after(1ms, [&] {
            timeoutSelected = true;
        }));
    assert(selectTimeout.isOk());
    assert(*selectTimeout == 2);
    assert(timeoutSelected);

    selectA.close();
    auto closedSelection = lgc::select(
        lgc::recv(selectAReceiver, [&](lgc::ChannelReceive<int> received) {
            selectedClosed = received.closed_;
        }),
        lgc::recv(selectBReceiver, [](lgc::ChannelReceive<int>) {
            assert(false);
        }));
    assert(closedSelection.isOk());
    assert(*closedSelection == 0);
    assert(selectedClosed);

    lgc::BoundedChannel<int> selectSendChannel(1);
    bool sendSelected = false;
    auto sendSelection = lgc::select(
        lgc::send(selectSendChannel.sender(), 7, [&] {
            sendSelected = true;
        }),
        lgc::after(1s, [] {
            assert(false);
        }));
    assert(sendSelection.isOk());
    assert(*sendSelection == 0);
    assert(sendSelected);
    auto sentValue = selectSendChannel.receive();
    assert(sentValue.isOk());
    assert(sentValue->has_value());
    assert(**sentValue == 7);

    assert(selectSendChannel.send(1).isOk());
    bool otherwiseSelected = false;
    auto defaultSelection = lgc::select(
        lgc::send(selectSendChannel.sender(), 2, [] {
            assert(false);
        }),
        lgc::otherwise([&] {
            otherwiseSelected = true;
        }));
    assert(defaultSelection.isOk());
    assert(*defaultSelection == 1);
    assert(otherwiseSelected);
    auto retainedValue = selectSendChannel.receive();
    assert(retainedValue.isOk());
    assert(retainedValue->has_value());
    assert(**retainedValue == 1);

    lgc::CancellationSource selectCancel;
    assert(selectCancel.cancel("select cancelled"));
    bool cancelSelected = false;
    auto cancelSelection = lgc::select(
        lgc::cancelled(selectCancel.token(), [&](const std::string& reason) {
            cancelSelected = reason == "select cancelled";
        }),
        lgc::after(1s, [] {
            assert(false);
        }));
    assert(cancelSelection.isOk());
    assert(*cancelSelection == 0);
    assert(cancelSelected);

    lgc::BoundedChannel<int> numberChannel(1);
    lgc::BoundedChannel<std::string> textChannel(1);
    assert(textChannel.send("ready").isOk());
    std::string textValue;
    auto heterogeneousSelection = lgc::select(
        lgc::recv(numberChannel.receiver(), [](int) {
            assert(false);
        }),
        lgc::recv(textChannel.receiver(), [&](std::string value) {
            textValue = std::move(value);
        }));
    assert(heterogeneousSelection.isOk());
    assert(*heterogeneousSelection == 1);
    assert(textValue == "ready");

    lgc::BoundedChannel<std::unique_ptr<int>> moveOnlyChannel(1);
    auto moveOnlySelection = lgc::select(lgc::send(moveOnlyChannel.sender(), std::make_unique<int>(31)));
    assert(moveOnlySelection.isOk());
    assert(*moveOnlySelection == 0);
    auto moveOnlyItem = moveOnlyChannel.receive();
    assert(moveOnlyItem.isOk());
    assert(moveOnlyItem->has_value());
    assert(***moveOnlyItem == 31);

    return 0;
}
