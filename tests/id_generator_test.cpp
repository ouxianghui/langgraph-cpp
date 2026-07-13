#include "foundation/id/id_generator.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

class FixedRandomSource final : public lc::IRandomSource {
public:
    explicit FixedRandomSource(std::byte seed = std::byte { 0 })
        : seed_(seed)
    {
    }

    [[nodiscard]] lc::Result<void> fill(std::span<std::byte> data) override
    {
        const auto base = counter_.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<std::byte>(
                (static_cast<unsigned char>(seed_) + ((base >> ((i % 8U) * 8U)) & 0xffU) + i) & 0xffU);
        return lc::okResult();
    }

private:
    std::byte seed_;
    std::atomic<std::uint64_t> counter_ { 0 };
};

class FailingRandomSource final : public lc::IRandomSource {
public:
    [[nodiscard]] lc::Result<void> fill(std::span<std::byte>) override
    {
        return lc::Status::unavailable("random source unavailable");
    }
};

class ConstantRandomSource final : public lc::IRandomSource {
public:
    explicit ConstantRandomSource(std::byte value)
        : value_(value)
    {
    }

    [[nodiscard]] lc::Result<void> fill(std::span<std::byte> data) override
    {
        std::ranges::fill(data, value_);
        return lc::okResult();
    }

private:
    std::byte value_;
};

class FixedIdClock final : public lc::IIdClock {
public:
    explicit FixedIdClock(std::chrono::milliseconds::rep value)
        : value_(value)
    {
    }

    [[nodiscard]] lc::Result<std::chrono::milliseconds::rep> unixTimeMs() const override
    {
        return value_.load(std::memory_order_relaxed);
    }

    void set(std::chrono::milliseconds::rep value)
    {
        value_.store(value, std::memory_order_relaxed);
    }

private:
    std::atomic<std::chrono::milliseconds::rep> value_;
};

class FailingIdClock final : public lc::IIdClock {
public:
    [[nodiscard]] lc::Result<std::chrono::milliseconds::rep> unixTimeMs() const override
    {
        return lc::Status::unavailable("clock unavailable");
    }
};

[[nodiscard]] bool isLowerHex(char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool isCrockfordBase32(char value)
{
    constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    return alphabet.find(value) != std::string_view::npos;
}

void verifyUuid()
{
    auto generator = lc::IdGenerator::uuidV4("run");
    const auto id = generator.next();
    assert(id.isOk());

    assert(generator.status().isOk());
    assert(generator.strategy() == lc::IdStrategy::UuidV4);
    assert(generator.prefix() == "run");
    assert(generator.separator() == "_");
    assert(id->starts_with("run_"));

    const auto raw = id->substr(4);
    assert(raw.size() == 36);
    assert(raw[8] == '-');
    assert(raw[13] == '-');
    assert(raw[18] == '-');
    assert(raw[23] == '-');
    assert(raw[14] == '4');
    assert(raw[19] == '8' || raw[19] == '9' || raw[19] == 'a' || raw[19] == 'b');

    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            continue;
        assert(isLowerHex(raw[i]));
    }
}

void verifyUlid()
{
    auto clock = std::make_shared<FixedIdClock>(1'700'000'000'000);
    auto random = std::make_shared<FixedRandomSource>();
    lc::IdGenerator generator(lc::IdGeneratorOptions {
        .strategy_ = lc::IdStrategy::Ulid,
        .prefix_ = "thread",
        .randomSource_ = random,
        .clock_ = clock,
    });

    const auto first = generator.next();
    const auto second = generator.next();
    const auto checkpoint = generator.next("checkpoint");
    assert(first.isOk());
    assert(second.isOk());
    assert(checkpoint.isOk());

    assert(first->starts_with("thread_"));
    assert(second->starts_with("thread_"));
    assert(checkpoint->starts_with("checkpoint_"));
    assert(*first < *second);

    const auto raw = checkpoint->substr(std::string("checkpoint_").size());
    assert(raw.size() == 26);
    assert(std::all_of(raw.begin(), raw.end(), isCrockfordBase32));

    clock->set(1'699'999'999'999);
    const auto afterClockRollback = generator.next();
    assert(afterClockRollback.isOk());
    assert(*second < *afterClockRollback);
}

void verifyMonotonic()
{
    auto generator = lc::IdGenerator::monotonic("node_exec", 7);

    assert(generator.strategy() == lc::IdStrategy::Monotonic);
    auto first = generator.next();
    auto second = generator.next();
    auto checkpoint = generator.next("checkpoint");
    assert(first.isOk());
    assert(second.isOk());
    assert(checkpoint.isOk());
    assert(*first == "node_exec_7");
    assert(*second == "node_exec_8");
    assert(*checkpoint == "checkpoint_9");
}

void verifyConcurrent(lc::IdStrategy strategy)
{
    std::shared_ptr<lc::IRandomSource> random = std::make_shared<FixedRandomSource>();
    std::shared_ptr<lc::IIdClock> clock = std::make_shared<FixedIdClock>(1'700'000'000'000);
    lc::IdGenerator generator(lc::IdGeneratorOptions {
        .strategy_ = strategy,
        .prefix_ = "run",
        .randomSource_ = random,
        .clock_ = clock,
    });

    std::mutex mutex;
    std::unordered_set<std::string> ids;

    constexpr int kThreadCount = 8;
    constexpr int kIdsPerThread = 1000;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back([&] {
            std::vector<std::string> local;
            local.reserve(kIdsPerThread);

            for (int j = 0; j < kIdsPerThread; ++j) {
                auto id = generator.next();
                assert(id.isOk());
                local.push_back(std::move(*id));
            }

            std::lock_guard lock(mutex);
            for (auto& id : local)
                ids.insert(std::move(id));
        });
    }

    for (auto& thread : threads)
        thread.join();

    assert(ids.size() == static_cast<std::size_t>(kThreadCount * kIdsPerThread));
}

void verifyConcurrentMonotonic()
{
    auto generator = lc::IdGenerator::monotonic("run", 1);
    std::mutex mutex;
    std::unordered_set<std::string> ids;

    constexpr int kThreadCount = 8;
    constexpr int kIdsPerThread = 1000;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back([&] {
            std::vector<std::string> local;
            local.reserve(kIdsPerThread);

            for (int j = 0; j < kIdsPerThread; ++j) {
                auto id = generator.next();
                assert(id.isOk());
                local.push_back(std::move(*id));
            }

            std::lock_guard lock(mutex);
            for (auto& id : local)
                ids.insert(std::move(id));
        });
    }

    for (auto& thread : threads)
        thread.join();

    assert(ids.size() == static_cast<std::size_t>(kThreadCount * kIdsPerThread));
}

void verifyFailurePaths()
{
    {
        lc::IdGenerator generator(lc::IdGeneratorOptions {
            .strategy_ = lc::IdStrategy::UuidV4,
            .randomSource_ = std::make_shared<FailingRandomSource>(),
        });
        auto id = generator.next();
        assert(!id.isOk());
        assert(id.status().code() == lc::StatusCode::Unavailable);
    }

    {
        lc::IdGenerator generator(lc::IdGeneratorOptions {
            .strategy_ = lc::IdStrategy::Ulid,
            .randomSource_ = std::make_shared<FailingRandomSource>(),
            .clock_ = std::make_shared<FixedIdClock>(1'700'000'000'000),
        });
        auto id = generator.next();
        assert(!id.isOk());
        assert(id.status().code() == lc::StatusCode::Unavailable);
    }

    {
        lc::IdGenerator generator(lc::IdGeneratorOptions {
            .strategy_ = lc::IdStrategy::Ulid,
            .randomSource_ = std::make_shared<FixedRandomSource>(),
            .clock_ = std::make_shared<FailingIdClock>(),
        });
        auto id = generator.next();
        assert(!id.isOk());
        assert(id.status().code() == lc::StatusCode::Unavailable);
    }

    {
        lc::IdGenerator generator(lc::IdGeneratorOptions {
            .strategy_ = lc::IdStrategy::Ulid,
            .randomSource_ = std::make_shared<FixedRandomSource>(),
            .clock_ = std::make_shared<FixedIdClock>(-1),
        });
        auto id = generator.next();
        assert(!id.isOk());
        assert(id.status().code() == lc::StatusCode::OutOfRange);
    }

    {
        lc::IdGenerator generator(lc::IdGeneratorOptions {
            .strategy_ = lc::IdStrategy::Ulid,
            .randomSource_ = std::make_shared<FixedRandomSource>(),
            .clock_ = std::make_shared<FixedIdClock>(static_cast<std::chrono::milliseconds::rep>(1ULL << 48U)),
        });
        auto id = generator.next();
        assert(!id.isOk());
        assert(id.status().code() == lc::StatusCode::OutOfRange);
    }

    {
        lc::IdGenerator generator(lc::IdGeneratorOptions {
            .strategy_ = lc::IdStrategy::Ulid,
            .randomSource_ = std::make_shared<ConstantRandomSource>(std::byte { 0xff }),
            .clock_ = std::make_shared<FixedIdClock>(1'700'000'000'000),
        });
        auto first = generator.next();
        assert(first.isOk());
        auto exhausted = generator.next();
        assert(!exhausted.isOk());
        assert(exhausted.status().code() == lc::StatusCode::ResourceExhausted);
    }

    {
        auto generator = lc::IdGenerator::monotonic({}, std::numeric_limits<std::uint64_t>::max());
        auto id = generator.next();
        assert(!id.isOk());
        assert(id.status().code() == lc::StatusCode::OutOfRange);
    }

    {
        lc::IdGenerator invalidPrefix(lc::IdGeneratorOptions {
            .prefix_ = "bad prefix",
        });
        assert(!invalidPrefix.status().isOk());
        assert(invalidPrefix.status().code() == lc::StatusCode::InvalidArgument);
        assert(!invalidPrefix.next().isOk());
    }

    {
        lc::IdGenerator invalidPrefix(lc::IdGeneratorOptions {
            .prefix_ = std::string(65, 'a'),
        });
        assert(!invalidPrefix.status().isOk());
        assert(invalidPrefix.status().code() == lc::StatusCode::InvalidArgument);
    }

    {
        lc::IdGenerator invalidSeparator(lc::IdGeneratorOptions {
            .separator_ = "/",
        });
        assert(!invalidSeparator.status().isOk());
        assert(invalidSeparator.status().code() == lc::StatusCode::InvalidArgument);
    }

    {
        lc::IdGenerator invalidSeparator(lc::IdGeneratorOptions {
            .separator_ = std::string(9, '_'),
        });
        assert(!invalidSeparator.status().isOk());
        assert(invalidSeparator.status().code() == lc::StatusCode::InvalidArgument);
    }

    {
        auto generator = lc::IdGenerator::uuidV4();
        auto id = generator.next("bad prefix");
        assert(!id.isOk());
        assert(id.status().code() == lc::StatusCode::InvalidArgument);
    }
}

} // namespace

int main()
{
    verifyUuid();
    verifyUlid();
    verifyMonotonic();
    verifyConcurrent(lc::IdStrategy::UuidV4);
    verifyConcurrent(lc::IdStrategy::Ulid);
    verifyConcurrentMonotonic();
    verifyFailurePaths();
    return 0;
}
