#pragma once

#include "foundation/async/channel_types.hpp"

namespace lgc {

class SelectOp final {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    SelectOp() = default;

private:
    friend Result<std::size_t> select(std::span<SelectOp> operations);
    template <typename T, typename Handler>
    friend SelectOp recv(ChannelReceiver<T> receiver, Handler handler);
    template <typename T, typename Handler>
    friend SelectOp send(ChannelSender<T> sender, T value, Handler handler);
    template <typename T>
    friend SelectOp send(ChannelSender<T> sender, T value);
    template <typename Rep, typename Period, typename Handler>
    friend SelectOp after(std::chrono::duration<Rep, Period> delay, Handler handler);
    template <typename Rep, typename Period>
    friend SelectOp after(std::chrono::duration<Rep, Period> delay);
    template <typename Handler>
    friend SelectOp cancelled(const CancellationToken& token, Handler handler);
    friend SelectOp cancelled(const CancellationToken& token);
    template <typename Handler>
    friend SelectOp otherwise(Handler handler);
    friend SelectOp otherwise();

    using AttachFn = std::function<Status(const std::shared_ptr<detail::ChannelWaiter>&)>;
    using TryRunFn = std::function<detail::SelectAttempt()>;
    using DeadlineFn = std::function<std::optional<TimePoint>()>;

    SelectOp(AttachFn attach, TryRunFn tryRun, DeadlineFn deadline, bool otherwise)
        : attach_(std::move(attach))
        , tryRun_(std::move(tryRun))
        , deadline_(std::move(deadline))
        , otherwise_(otherwise)
    {
    }

    AttachFn attach_;
    TryRunFn tryRun_;
    DeadlineFn deadline_;
    bool otherwise_ { false };
};

[[nodiscard]] inline Result<std::size_t> select(std::span<SelectOp> operations)
{
    if (operations.empty())
        return Status::invalidArgument("select requires at least one operation");

    std::optional<std::size_t> otherwiseIndex;
    for (std::size_t i = 0; i < operations.size(); ++i) {
        if (!operations[i].tryRun_)
            return Status::invalidArgument("select operation is not initialized");
        if (operations[i].otherwise_) {
            if (otherwiseIndex.has_value())
                return Status::invalidArgument("select allows at most one otherwise operation");
            otherwiseIndex = i;
        }
    }

    auto waiter = std::make_shared<detail::ChannelWaiter>();
    std::vector<std::size_t> operationOrder;
    operationOrder.reserve(operations.size());
    for (std::size_t index = 0; index < operations.size(); ++index) {
        auto& operation = operations[index];
        if (operation.otherwise_)
            continue;
        operationOrder.push_back(index);
        if (operation.attach_) {
            if (auto status = operation.attach_(waiter); !status.isOk())
                return status;
        }
    }

    auto runReadyOperation = [&]() -> Result<std::optional<std::size_t>> {
        detail::shuffleSelectOrder(operationOrder);
        for (const auto index : operationOrder) {
            auto attempt = operations[index].tryRun_();
            if (!attempt.status_.isOk())
                return attempt.status_;
            if (attempt.ready_)
                return std::optional<std::size_t>(index);
        }
        return std::optional<std::size_t> {};
    };

    for (;;) {
        const auto observed = waiter->generation();
        auto ready = runReadyOperation();
        if (!ready.isOk())
            return ready.status();
        if (ready->has_value())
            return **ready;

        if (otherwiseIndex.has_value()) {
            auto attempt = operations[*otherwiseIndex].tryRun_();
            if (!attempt.status_.isOk())
                return attempt.status_;
            return *otherwiseIndex;
        }

        std::optional<SelectOp::TimePoint> nextDeadline;
        for (const auto& operation : operations) {
            if (!operation.deadline_)
                continue;
            auto deadline = operation.deadline_();
            if (!deadline.has_value())
                continue;
            if (!nextDeadline.has_value() || *deadline < *nextDeadline)
                nextDeadline = *deadline;
        }

        if (!nextDeadline.has_value()) {
            waiter->wait(observed);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= *nextDeadline)
            continue;
        (void)waiter->waitFor(observed, *nextDeadline - now);
    }
}

template <typename... Operations>
[[nodiscard]] Result<std::size_t> select(Operations&&... operations)
{
    std::vector<SelectOp> selectOperations;
    selectOperations.reserve(sizeof...(Operations));
    (selectOperations.emplace_back(std::forward<Operations>(operations)), ...);
    return select(std::span<SelectOp>(selectOperations));
}

} // namespace lgc
