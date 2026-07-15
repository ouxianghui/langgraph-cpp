#pragma once

#include "foundation/async/future.hpp"
#include "foundation/executor/i_executor.hpp"
#include "langgraph/checkpoint/saver.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace lgc {

/// Future-returning checkpointer facade for runtimes that need async saver variants.
///
/// With an executor, operations are posted and complete through Future<T>. Without one, operations
/// execute on the caller thread and still use the same future contract, which is useful in tests.
class AsyncCheckpointSaver final {
public:
    explicit AsyncCheckpointSaver(
        std::shared_ptr<BaseCheckpointSaver> checkpointer,
        std::shared_ptr<IExecutor> executor = {});

    [[nodiscard]] Future<void> put(Checkpoint checkpoint) const;
    [[nodiscard]] Future<void> putWrites(CheckpointWriteSet writes) const;
    [[nodiscard]] Future<std::optional<Checkpoint>> get(CheckpointQuery query) const;
    [[nodiscard]] Future<std::optional<CheckpointTuple>> getTuple(CheckpointQuery query) const;
    [[nodiscard]] Future<std::vector<CheckpointTuple>> list(CheckpointListOptions options) const;
    [[nodiscard]] Future<void> deleteThread(std::string threadId) const;
    [[nodiscard]] Future<CheckpointMaintenanceResult> prune(
        std::string threadId,
        CheckpointPruneOptions options) const;
    [[nodiscard]] Future<CheckpointMaintenanceResult> copyThread(
        CheckpointCopyThreadOptions options) const;
    [[nodiscard]] Future<CheckpointMaintenanceResult> deleteForRuns(
        CheckpointDeleteForRunsOptions options) const;
    [[nodiscard]] Future<DeltaChannelHistories> getDeltaChannelHistory(
        DeltaChannelHistoryQuery query) const;

private:
    template <typename T, typename Fn>
    [[nodiscard]] Future<T> submit(Fn&& fn) const;
    template <typename Fn>
    [[nodiscard]] Future<void> submitVoid(Fn&& fn) const;

    std::shared_ptr<BaseCheckpointSaver> checkpointer_;
    std::shared_ptr<IExecutor> executor_;
};

template <typename T, typename Fn>
Future<T> AsyncCheckpointSaver::submit(Fn&& fn) const
{
    auto promise = std::make_shared<Promise<T>>();
    auto future = promise->future();
    if (!checkpointer_) {
        (void)promise->reject(Status::invalidArgument("async checkpointer requires a checkpointer"));
        return future;
    }

    auto task = [promise, fn = std::forward<Fn>(fn)]() mutable {
        try {
            auto result = fn();
            if (!result.isOk()) {
                (void)promise->reject(result.status());
                return;
            }
            (void)promise->resolve(std::move(*result));
        } catch (const std::exception& error) {
            (void)promise->reject(Status::internal(std::string("async checkpointer task threw: ") + error.what()));
        } catch (...) {
            (void)promise->reject(Status::internal("async checkpointer task threw an unknown exception"));
        }
    };

    if (!executor_) {
        task();
        return future;
    }

    if (auto posted = executor_->post(std::move(task)); !posted.isOk())
        (void)promise->reject(posted);
    return future;
}

template <typename Fn>
Future<void> AsyncCheckpointSaver::submitVoid(Fn&& fn) const
{
    auto promise = std::make_shared<Promise<void>>();
    auto future = promise->future();
    if (!checkpointer_) {
        (void)promise->reject(Status::invalidArgument("async checkpointer requires a checkpointer"));
        return future;
    }

    auto task = [promise, fn = std::forward<Fn>(fn)]() mutable {
        try {
            auto result = fn();
            if (!result.isOk()) {
                (void)promise->reject(result.status());
                return;
            }
            (void)promise->resolve();
        } catch (const std::exception& error) {
            (void)promise->reject(Status::internal(std::string("async checkpointer task threw: ") + error.what()));
        } catch (...) {
            (void)promise->reject(Status::internal("async checkpointer task threw an unknown exception"));
        }
    };

    if (!executor_) {
        task();
        return future;
    }

    if (auto posted = executor_->post(std::move(task)); !posted.isOk())
        (void)promise->reject(posted);
    return future;
}

} // namespace lgc
