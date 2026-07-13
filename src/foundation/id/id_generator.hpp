#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace lc {

enum class IdStrategy : std::uint8_t {
    UuidV4,
    Ulid,
    Monotonic,
};

class IRandomSource {
public:
    virtual ~IRandomSource() = default;

    [[nodiscard]] virtual Result<void> fill(std::span<std::byte> data) = 0;
};

class IIdClock {
public:
    virtual ~IIdClock() = default;

    [[nodiscard]] virtual Result<std::chrono::milliseconds::rep> unixTimeMs() const = 0;
};

struct IdGeneratorOptions {
    IdStrategy strategy_ { IdStrategy::Ulid };
    std::string prefix_;
    std::string separator_ { "_" };
    std::uint64_t monotonicStart_ { 1 };
    std::shared_ptr<IRandomSource> randomSource_;
    std::shared_ptr<IIdClock> clock_;
    std::size_t maxPrefixLength_ { 64 };
    std::size_t maxSeparatorLength_ { 8 };
};

[[nodiscard]] Status validateIdGeneratorOptions(const IdGeneratorOptions& options);

class OsRandomSource final : public IRandomSource {
public:
    [[nodiscard]] Result<void> fill(std::span<std::byte> data) override;
};

class SystemIdClock final : public IIdClock {
public:
    [[nodiscard]] static std::shared_ptr<IIdClock> instance();

    [[nodiscard]] Result<std::chrono::milliseconds::rep> unixTimeMs() const override;
};

class IdGenerator final {
public:
    explicit IdGenerator(IdGeneratorOptions options = {});
    IdGenerator(const IdGenerator&) = delete;
    IdGenerator& operator=(const IdGenerator&) = delete;
    IdGenerator(IdGenerator&&) = delete;
    IdGenerator& operator=(IdGenerator&&) = delete;
    ~IdGenerator() = default;

    [[nodiscard]] static IdGenerator uuidV4(std::string prefix = {});
    [[nodiscard]] static IdGenerator ulid(std::string prefix = {});
    [[nodiscard]] static IdGenerator monotonic(std::string prefix = {}, std::uint64_t start = 1);

    [[nodiscard]] Result<std::string> next();
    [[nodiscard]] Result<std::string> next(std::string_view prefixOverride);

    [[nodiscard]] IdStrategy strategy() const noexcept;
    [[nodiscard]] const std::string& prefix() const noexcept;
    [[nodiscard]] const std::string& separator() const noexcept;
    [[nodiscard]] const Status& status() const noexcept;

private:
    [[nodiscard]] Status validatePrefix(std::string_view prefix) const;
    [[nodiscard]] Result<std::string> nextRaw();
    [[nodiscard]] Result<std::string> withPrefix(std::string_view prefix, std::string raw) const;
    [[nodiscard]] Result<std::string> nextUuidV4();
    [[nodiscard]] Result<std::string> nextUlid();
    [[nodiscard]] Result<std::string> nextMonotonic();

    IdGeneratorOptions options_;
    Status status_;
    std::shared_ptr<IRandomSource> randomSource_;
    std::shared_ptr<IIdClock> clock_;
    std::atomic<std::uint64_t> counter_;

    std::mutex ulidMutex_;
    std::chrono::milliseconds::rep lastUlidTimeMs_ { -1 };
    std::uint8_t lastUlidRandom_[10] {};
};

} // namespace lc
