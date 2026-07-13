#include "langgraph/checkpoint/checkpoint_common.hh"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace lc {

using namespace detail;

StorageSaver::StorageSaver(
    std::shared_ptr<IStorage> storage,
    StorageSaverOptions options)
    : storage_(std::move(storage))
    , options_(std::move(options))
{
}

Result<StorageKey> StorageSaver::keyFor(
    std::string_view threadId,
    std::string_view checkpointNamespace,
    std::string_view checkpointId) const
{
    if (threadId.empty())
        return Status::invalidArgument("checkpoint thread_id cannot be empty");
    if (checkpointId.empty())
        return Status::invalidArgument("checkpoint checkpoint_id cannot be empty");

    auto prefix = keyPrefixFor(threadId, checkpointNamespace);
    if (!prefix.isOk())
        return prefix.status();

    return StorageKey {
        .scope_ = options_.scope_,
        .key_ = *prefix + hexEncode(checkpointId),
    };
}

Result<std::string> StorageSaver::keyPrefixFor(
    std::string_view threadId,
    std::string_view checkpointNamespace) const
{
    if (threadId.empty())
        return Status::invalidArgument("checkpoint thread_id cannot be empty");
    std::string prefix = hexEncode(threadId);
    prefix.push_back('/');
    if (!checkpointNamespace.empty()) {
        prefix.append("ns/");
        prefix.append(hexEncode(checkpointNamespace));
        prefix.push_back('/');
    }
    return prefix;
}

Result<StorageKey> StorageSaver::latestPointerKeyFor(
    std::string_view threadId,
    std::string_view checkpointNamespace) const
{
    if (threadId.empty())
        return Status::invalidArgument("checkpoint thread_id cannot be empty");
    std::string key = "latest/" + hexEncode(threadId);
    if (!checkpointNamespace.empty()) {
        key.append("/ns/");
        key.append(hexEncode(checkpointNamespace));
    }
    return StorageKey {
        .scope_ = options_.scope_,
        .key_ = std::move(key),
    };
}

Result<std::string> StorageSaver::writeKeyPrefixFor(
    std::string_view threadId,
    std::string_view checkpointNamespace,
    std::string_view checkpointId) const
{
    if (threadId.empty())
        return Status::invalidArgument("checkpoint writes thread_id cannot be empty");
    if (checkpointId.empty())
        return Status::invalidArgument("checkpoint writes checkpoint_id cannot be empty");

    std::string key = "writes/";
    key.append(hexEncode(threadId));
    key.push_back('/');
    if (!checkpointNamespace.empty()) {
        key.append("ns/");
        key.append(hexEncode(checkpointNamespace));
        key.push_back('/');
    }
    key.append("checkpoint/");
    key.append(hexEncode(checkpointId));
    key.push_back('/');
    return key;
}

Result<StorageKey> StorageSaver::writeKeyFor(
    const CheckpointWriteSet& writes,
    const CheckpointWrite& write,
    std::size_t index) const
{
    auto prefix = writeKeyPrefixFor(
        writes.threadId_,
        writes.checkpointNamespace_,
        writes.checkpointId_);
    if (!prefix.isOk())
        return prefix.status();

    const auto order = write.order_.value_or(static_cast<std::uint64_t>(index));
    std::string key = *prefix;
    key.append(hexEncode(write.taskId_));
    key.push_back('/');
    key.append(hexEncode(write.taskPath_));
    key.push_back('/');
    key.append(paddedIndex(order));
    return StorageKey {
        .scope_ = options_.scope_,
        .key_ = std::move(key),
    };
}

Result<void> StorageSaver::put(Checkpoint checkpoint)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();
    if (auto result = validateCheckpointForStore(checkpoint); !result.isOk())
        return result.status();

    auto key = keyFor(
        checkpoint.threadId_,
        checkpoint.checkpointNamespace_,
        checkpoint.checkpointId_);
    if (!key.isOk())
        return key.status();

    auto payload = options_.codec_->encode(checkpoint);
    if (!payload.isOk())
        return payload.status();
    auto value = storageValueFromPayload(*payload);
    if (!value.isOk())
        return value.status();

    if (auto stored = storage_->put(
            std::move(*key),
            std::move(*value),
            StoragePutOptions { .mode_ = StoragePutMode::InsertOnly });
        !stored.isOk()) {
        return stored.status();
    }

    auto pointerKey = latestPointerKeyFor(checkpoint.threadId_, checkpoint.checkpointNamespace_);
    if (!pointerKey.isOk())
        return pointerKey.status();
    return storage_->put(
        std::move(*pointerKey),
        checkpoint.checkpointId_,
        StoragePutOptions { .mode_ = StoragePutMode::Upsert });
}

Result<void> StorageSaver::putWrites(CheckpointWriteSet writes)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();
    auto normalized = normalizeWriteSet(std::move(writes));
    if (!normalized.isOk())
        return normalized.status();

    for (std::size_t i = 0; i < normalized->writes_.size(); ++i) {
        auto key = writeKeyFor(*normalized, normalized->writes_[i], i);
        if (!key.isOk())
            return key.status();
        auto payload = options_.codec_->encodeWrite(normalized->writes_[i]);
        if (!payload.isOk())
            return payload.status();
        auto value = storageValueFromPayload(*payload);
        if (!value.isOk())
            return value.status();
        if (auto stored = storage_->put(
                std::move(*key),
                std::move(*value),
                StoragePutOptions { .mode_ = StoragePutMode::Upsert });
            !stored.isOk()) {
            return stored.status();
        }
    }
    return okResult();
}

Result<std::optional<Checkpoint>> StorageSaver::getLatestCheckpoint(
    std::string_view threadId,
    std::string_view checkpointNamespace)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();

    auto pointerKey = latestPointerKeyFor(threadId, checkpointNamespace);
    if (!pointerKey.isOk())
        return pointerKey.status();

    auto pointer = storage_->get(*pointerKey);
    if (!pointer.isOk())
        return pointer.status();
    if (!pointer->has_value())
        return getLatestByScan(threadId, checkpointNamespace);

    auto checkpoint = getCheckpoint(threadId, (*pointer)->value_, checkpointNamespace);
    if (!checkpoint.isOk())
        return checkpoint.status();
    if (!checkpoint->has_value())
        return getLatestByScan(threadId, checkpointNamespace);

    auto scanned = getLatestByScan(threadId, checkpointNamespace);
    if (!scanned.isOk())
        return scanned.status();
    if (scanned->has_value() && checkpointIsNewer(**scanned, **checkpoint))
        return scanned;
    return checkpoint;
}

Result<std::optional<Checkpoint>> StorageSaver::getLatestByScan(
    std::string_view threadId,
    std::string_view checkpointNamespace)
{
    auto checkpoints = listCheckpoints(threadId, checkpointNamespace);
    if (!checkpoints.isOk())
        return checkpoints.status();
    if (checkpoints->empty())
        return std::optional<Checkpoint> {};
    return std::optional<Checkpoint>(checkpoints->back());
}

Result<std::optional<Checkpoint>> StorageSaver::getCheckpoint(
    std::string_view threadId,
    std::string_view checkpointId,
    std::string_view checkpointNamespace)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();

    auto key = keyFor(threadId, checkpointNamespace, checkpointId);
    if (!key.isOk())
        return key.status();

    auto item = storage_->get(*key);
    if (!item.isOk())
        return item.status();
    if (!item->has_value())
        return std::optional<Checkpoint> {};

    auto payload = payloadFromStorageValue((*item)->value_);
    if (!payload.isOk())
        return payload.status();
    auto checkpoint = options_.codec_->decode(*payload);
    if (!checkpoint.isOk())
        return checkpoint.status();
    auto writes = readWrites(threadId, checkpointNamespace, checkpointId);
    if (!writes.isOk())
        return writes.status();
    if (checkpoint->pendingWrites_.empty())
        checkpoint->pendingWrites_ = std::move(*writes);
    return std::optional<Checkpoint>(*checkpoint);
}

Result<std::vector<Checkpoint>> StorageSaver::listCheckpoints(
    std::string_view threadId,
    std::string_view checkpointNamespace)
{
    std::vector<Checkpoint> checkpoints;
    auto records = list(CheckpointListOptions {
        .threadId_ = std::string(threadId),
        .checkpointNamespace_ = std::string(checkpointNamespace),
        .order_ = CheckpointListOrder::OldestFirst,
    });
    if (!records.isOk())
        return records.status();
    checkpoints.reserve(records->size());
    for (auto& record : *records)
        checkpoints.push_back(std::move(record.checkpoint_));
    return checkpoints;
}

Result<std::vector<CheckpointWrite>> StorageSaver::readWrites(
    std::string_view threadId,
    std::string_view checkpointNamespace,
    std::string_view checkpointId) const
{
    auto prefix = writeKeyPrefixFor(threadId, checkpointNamespace, checkpointId);
    if (!prefix.isOk())
        return prefix.status();

    std::vector<CheckpointWrite> writes;
    std::string cursor;
    for (;;) {
        auto page = storage_->list(StorageListOptions {
            .scope_ = options_.scope_,
            .keyPrefix_ = *prefix,
            .limit_ = options_.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();

        for (const auto& item : page->items_) {
            auto payload = payloadFromStorageValue(item.value_);
            if (!payload.isOk())
                return payload.status();
            auto write = options_.codec_->decodeWrite(*payload);
            if (!write.isOk())
                return write.status();
            writes.push_back(std::move(*write));
        }

        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }

    std::stable_sort(
        writes.begin(),
        writes.end(),
        [](const CheckpointWrite& lhs, const CheckpointWrite& rhs) {
            if (lhs.taskId_ != rhs.taskId_)
                return lhs.taskId_ < rhs.taskId_;
            if (lhs.taskPath_ != rhs.taskPath_)
                return lhs.taskPath_ < rhs.taskPath_;
            return lhs.order_.value_or(std::numeric_limits<std::uint64_t>::max())
                < rhs.order_.value_or(std::numeric_limits<std::uint64_t>::max());
        });
    return writes;
}

Result<std::optional<Checkpoint>> StorageSaver::get(CheckpointQuery query)
{
    auto tuple = getTuple(std::move(query));
    if (!tuple.isOk())
        return tuple.status();
    if (!tuple->has_value())
        return std::optional<Checkpoint> {};
    return std::optional<Checkpoint>(std::move((*tuple)->checkpoint_));
}

Result<std::optional<CheckpointTuple>> StorageSaver::getTuple(CheckpointQuery query)
{
    if (auto result = validateCheckpointQuery(query); !result.isOk())
        return result.status();

    auto checkpoint = query.checkpointId_.has_value()
        ? getCheckpoint(query.threadId_, *query.checkpointId_, query.checkpointNamespace_)
        : getLatestCheckpoint(query.threadId_, query.checkpointNamespace_);
    if (!checkpoint.isOk())
        return checkpoint.status();
    if (!checkpoint->has_value())
        return std::optional<CheckpointTuple> {};

    auto writes = readWrites(
        (*checkpoint)->threadId_,
        (*checkpoint)->checkpointNamespace_,
        (*checkpoint)->checkpointId_);
    if (!writes.isOk())
        return writes.status();
    std::vector<CheckpointWrite> pendingWrites = *writes;
    if (pendingWrites.empty())
        pendingWrites = (*checkpoint)->pendingWrites_;
    return std::optional<CheckpointTuple>(CheckpointTuple {
        .checkpoint_ = std::move(**checkpoint),
        .pendingWrites_ = std::move(pendingWrites),
    });
}

Result<std::vector<CheckpointTuple>> StorageSaver::list(CheckpointListOptions options)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();
    if (auto result = validateCheckpointListOptions(options); !result.isOk())
        return result.status();

    auto prefix = keyPrefixFor(
        options.threadId_,
        options.checkpointNamespace_.value_or(std::string()));
    if (!prefix.isOk())
        return prefix.status();

    std::vector<Checkpoint> checkpoints;
    std::string cursor;
    for (;;) {
        auto page = storage_->list(StorageListOptions {
            .scope_ = options_.scope_,
            .keyPrefix_ = *prefix,
            .limit_ = options_.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();

        for (const auto& item : page->items_) {
            auto payload = payloadFromStorageValue(item.value_);
            if (!payload.isOk())
                return payload.status();
            if (!isCheckpointPayload(payload->contentType_))
                continue;
            auto checkpoint = options_.codec_->decode(*payload);
            if (!checkpoint.isOk())
                return checkpoint.status();
            if (options.checkpointNamespace_.has_value()
                && checkpoint->checkpointNamespace_ != *options.checkpointNamespace_) {
                continue;
            }
            checkpoints.push_back(std::move(*checkpoint));
        }

        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }

    std::map<std::string, std::vector<CheckpointWrite>> writesByCheckpoint;
    for (const auto& checkpoint : checkpoints) {
        auto writes = readWrites(
            checkpoint.threadId_,
            checkpoint.checkpointNamespace_,
            checkpoint.checkpointId_);
        if (!writes.isOk())
            return writes.status();
        if (!writes->empty()) {
            writesByCheckpoint.emplace(
                checkpointWriteStorageKey(
                    checkpoint.threadId_,
                    checkpoint.checkpointNamespace_,
                    checkpoint.checkpointId_),
                std::move(*writes));
        }
    }

    return applyListOptions(std::move(checkpoints), writesByCheckpoint, options);
}

Result<void> StorageSaver::deleteThread(std::string_view threadId)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();

    auto prefix = keyPrefixFor(threadId, {});
    if (!prefix.isOk())
        return prefix.status();

    std::vector<StorageKey> keys;
    std::string cursor;
    for (;;) {
        auto page = storage_->list(StorageListOptions {
            .scope_ = options_.scope_,
            .keyPrefix_ = *prefix,
            .limit_ = options_.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();
        for (const auto& item : page->items_)
            keys.push_back(item.key_);
        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }

    for (const auto& key : keys) {
        if (auto removed = storage_->remove(key); !removed.isOk())
            return removed.status();
    }

    std::vector<StorageKey> pointerKeys;
    auto pointerPrefix = std::string("latest/") + hexEncode(threadId);
    cursor.clear();
    for (;;) {
        auto page = storage_->list(StorageListOptions {
            .scope_ = options_.scope_,
            .keyPrefix_ = pointerPrefix,
            .limit_ = options_.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();
        for (const auto& item : page->items_) {
            if (item.key_.key_ == pointerPrefix || item.key_.key_.starts_with(pointerPrefix + "/"))
                pointerKeys.push_back(item.key_);
        }
        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }

    for (const auto& key : pointerKeys) {
        if (auto removed = storage_->remove(key); !removed.isOk())
            return removed.status();
    }
    if (auto removed = removeThreadWrites(threadId); !removed.isOk())
        return removed.status();
    return okResult();
}

Result<void> StorageSaver::removeWrites(
    std::string_view threadId,
    std::string_view checkpointNamespace,
    std::string_view checkpointId)
{
    auto prefix = writeKeyPrefixFor(threadId, checkpointNamespace, checkpointId);
    if (!prefix.isOk())
        return prefix.status();

    std::vector<StorageKey> keys;
    std::string cursor;
    for (;;) {
        auto page = storage_->list(StorageListOptions {
            .scope_ = options_.scope_,
            .keyPrefix_ = *prefix,
            .limit_ = options_.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();
        for (const auto& item : page->items_)
            keys.push_back(item.key_);
        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }
    for (const auto& key : keys) {
        if (auto removed = storage_->remove(key); !removed.isOk())
            return removed.status();
    }
    return okResult();
}

Result<void> StorageSaver::removeThreadWrites(std::string_view threadId)
{
    if (threadId.empty())
        return Status::invalidArgument("checkpoint writes thread_id cannot be empty");
    const std::string prefix = "writes/" + hexEncode(threadId) + "/";

    std::vector<StorageKey> keys;
    std::string cursor;
    for (;;) {
        auto page = storage_->list(StorageListOptions {
            .scope_ = options_.scope_,
            .keyPrefix_ = prefix,
            .limit_ = options_.listPageSize_,
            .cursor_ = cursor,
        });
        if (!page.isOk())
            return page.status();
        for (const auto& item : page->items_)
            keys.push_back(item.key_);
        if (page->nextCursor_.empty())
            break;
        cursor = std::move(page->nextCursor_);
    }
    for (const auto& key : keys) {
        if (auto removed = storage_->remove(key); !removed.isOk())
            return removed.status();
    }
    return okResult();
}

Result<CheckpointMaintenanceResult> StorageSaver::prune(
    std::string_view threadId,
    const CheckpointPruneOptions& options)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();

    auto checkpoints = listCheckpoints(threadId, options.checkpointNamespace_);
    if (!checkpoints.isOk())
        return checkpoints.status();

    CheckpointMaintenanceResult result;
    if (checkpoints->size() > options.keepLatest_) {
        const auto removeCount = checkpoints->size() - options.keepLatest_;
        for (std::size_t i = 0; i < removeCount; ++i) {
            auto key = keyFor(
                (*checkpoints)[i].threadId_,
                (*checkpoints)[i].checkpointNamespace_,
                (*checkpoints)[i].checkpointId_);
            if (!key.isOk())
                return key.status();
            if (auto removed = storage_->remove(*key); !removed.isOk())
                return removed.status();
            if (auto removedWrites = removeWrites(
                    (*checkpoints)[i].threadId_,
                    (*checkpoints)[i].checkpointNamespace_,
                    (*checkpoints)[i].checkpointId_);
                !removedWrites.isOk()) {
                return removedWrites.status();
            }
            ++result.removed_;
        }
    }

    auto compacted = repairLatestPointer(threadId, options.checkpointNamespace_);
    if (!compacted.isOk())
        return compacted.status();
    result.remaining_ = compacted->remaining_;
    result.latestCheckpointId_ = compacted->latestCheckpointId_;
    return okResult(std::move(result));
}

Result<CheckpointMaintenanceResult> StorageSaver::deleteForRuns(
    CheckpointDeleteForRunsOptions options)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();
    if (auto result = validateDeleteForRunsOptions(options); !result.isOk())
        return result.status();

    auto records = list(CheckpointListOptions {
        .threadId_ = options.threadId_,
        .checkpointNamespace_ = options.checkpointNamespace_,
        .order_ = CheckpointListOrder::OldestFirst,
    });
    if (!records.isOk())
        return records.status();

    CheckpointMaintenanceResult result;
    std::set<std::string> affectedNamespaces;
    for (const auto& record : *records) {
        const auto& checkpoint = record.checkpoint_;
        if (!checkpointIdMatchesRun(checkpoint.checkpointId_, options.runIds_))
            continue;

        auto key = keyFor(
            checkpoint.threadId_,
            checkpoint.checkpointNamespace_,
            checkpoint.checkpointId_);
        if (!key.isOk())
            return key.status();
        if (auto removed = storage_->remove(*key); !removed.isOk())
            return removed.status();
        if (auto removedWrites = removeWrites(
                checkpoint.threadId_,
                checkpoint.checkpointNamespace_,
                checkpoint.checkpointId_);
            !removedWrites.isOk()) {
            return removedWrites.status();
        }
        affectedNamespaces.insert(checkpoint.checkpointNamespace_);
        ++result.removed_;
    }

    for (const auto& checkpointNamespace : affectedNamespaces) {
        auto compacted = repairLatestPointer(options.threadId_, checkpointNamespace);
        if (!compacted.isOk())
            return compacted.status();
        result.remaining_ += compacted->remaining_;
        if (!compacted->latestCheckpointId_.empty())
            result.latestCheckpointId_ = compacted->latestCheckpointId_;
    }
    return okResult(std::move(result));
}

Result<CheckpointMaintenanceResult> StorageSaver::repairLatestPointer(
    std::string_view threadId,
    std::string_view checkpointNamespace)
{
    if (auto result = validateStorageSaverOptions(storage_, options_); !result.isOk())
        return result.status();

    auto checkpoints = listCheckpoints(threadId, checkpointNamespace);
    if (!checkpoints.isOk())
        return checkpoints.status();

    auto pointerKey = latestPointerKeyFor(threadId, checkpointNamespace);
    if (!pointerKey.isOk())
        return pointerKey.status();

    CheckpointMaintenanceResult result {
        .remaining_ = checkpoints->size(),
    };
    if (checkpoints->empty()) {
        if (auto removed = storage_->remove(*pointerKey); !removed.isOk())
            return removed.status();
        return okResult(std::move(result));
    }

    result.latestCheckpointId_ = checkpoints->back().checkpointId_;
    if (auto stored = storage_->put(
            std::move(*pointerKey),
            result.latestCheckpointId_,
            StoragePutOptions { .mode_ = StoragePutMode::Upsert });
        !stored.isOk()) {
        return stored.status();
    }
    return okResult(std::move(result));
}

Result<CheckpointMaintenanceResult> StorageSaver::copyThread(
    CheckpointCopyThreadOptions options)
{
    return copyThreadWith(*this, std::move(options));
}

Result<DeltaChannelHistories> StorageSaver::getDeltaChannelHistory(
    DeltaChannelHistoryQuery query)
{
    return getDeltaChannelHistoryWith(*this, std::move(query));
}

} // namespace lc
