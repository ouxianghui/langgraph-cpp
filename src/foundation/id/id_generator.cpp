#include "foundation/id/id_generator.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bcrypt.h>
#include <windows.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <unistd.h>
#endif

namespace lgc {
namespace {

constexpr char kHexAlphabet[] = "0123456789abcdef";
constexpr char kCrockfordBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::uint64_t kUlidTimestampMask = 0x0000FFFFFFFFFFFFULL;
constexpr std::string_view kAllowedIdText = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-";
constexpr std::string_view kAllowedSeparatorText = "._-";

[[nodiscard]] Status validateText(
    std::string_view value,
    std::string_view field,
    std::size_t maxLength,
    std::string_view allowed)
{
    if (value.size() > maxLength) {
        std::string message("id generator ");
        message.append(field);
        message.append(" is too long");
        return Status::invalidArgument(std::move(message));
    }

    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) {
            std::string message("id generator ");
            message.append(field);
            message.append(" contains a control character");
            return Status::invalidArgument(std::move(message));
        }

        if (allowed.find(ch) == std::string_view::npos) {
            std::string message("id generator ");
            message.append(field);
            message.append(" contains a disallowed character");
            return Status::invalidArgument(std::move(message));
        }
    }

    return Status::ok();
}

[[maybe_unused]] [[nodiscard]] Result<void> fillRandomFromUrandom(std::span<std::byte> data) noexcept
{
    auto* file = std::fopen("/dev/urandom", "rb");
    if (file == nullptr)
        return Status::unavailable("failed to open /dev/urandom");

    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto remaining = data.size() - offset;
        const auto read = std::fread(
            data.data() + offset,
            1,
            remaining,
            file);
        if (read == 0) {
            const bool error = std::ferror(file) != 0;
            std::fclose(file);
            if (error)
                return Status::unavailable("failed to read /dev/urandom");
            return Status::unavailable("/dev/urandom returned EOF");
        }
        offset += read;
        if (read < remaining) {
            const bool error = std::ferror(file) != 0;
            const bool eof = std::feof(file) != 0;
            if (error || eof) {
                std::fclose(file);
                if (error)
                    return Status::unavailable("failed to read /dev/urandom");
                return Status::unavailable("/dev/urandom returned EOF");
            }
        }
    }

    std::fclose(file);
    return okResult();
}

[[nodiscard]] Result<void> fillRandomFromOs(std::span<std::byte> data) noexcept
{
    if (data.empty())
        return okResult();

#if defined(_WIN32)
    auto* cursor = reinterpret_cast<PUCHAR>(data.data());
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto chunk = std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<ULONG>::max()));
        const auto status = ::BCryptGenRandom(
            nullptr,
            cursor,
            static_cast<ULONG>(chunk),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0)
            return Status::unavailable("BCryptGenRandom failed");
        cursor += chunk;
        remaining -= chunk;
    }
    return okResult();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(data.data(), data.size());
    return okResult();
#elif defined(__linux__)
    auto* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto chunk = std::min<std::size_t>(remaining, 256U * 1024U);
        const auto read = ::getrandom(cursor, chunk, 0);
        if (read < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ENOSYS)
                return fillRandomFromUrandom(data);
            return Status::unavailable("getrandom failed: " + std::string(std::strerror(errno)));
        }
        if (read == 0)
            return Status::unavailable("getrandom returned no data");
        cursor += static_cast<std::size_t>(read);
        remaining -= static_cast<std::size_t>(read);
    }
    return okResult();
#else
    return fillRandomFromUrandom(data);
#endif
}

[[nodiscard]] bool incrementBigEndian(std::uint8_t* data, std::size_t size) noexcept
{
    for (std::size_t i = size; i > 0; --i) {
        auto& byte = data[i - 1];
        ++byte;
        if (byte != 0)
            return true;
    }
    return false;
}

[[nodiscard]] Status validateUlidTimeMs(std::chrono::milliseconds::rep timeMs)
{
    if (timeMs < 0)
        return Status::outOfRange("ULID timestamp cannot be negative");
    if (static_cast<std::uint64_t>(timeMs) > kUlidTimestampMask)
        return Status::outOfRange("ULID timestamp exceeds 48-bit range");
    return Status::ok();
}

[[nodiscard]] std::string encodeUlid(
    std::chrono::milliseconds::rep timeMs,
    const std::uint8_t* random)
{
    auto timestamp = static_cast<std::uint64_t>(timeMs) & kUlidTimestampMask;
    std::string out(26, '0');

    for (int i = 9; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kCrockfordBase32[timestamp & 0x1FU];
        timestamp >>= 5U;
    }

    int bitIndex = 0;
    for (int i = 10; i < 26; ++i) {
        std::uint8_t value = 0;
        for (int bit = 0; bit < 5; ++bit) {
            const auto byteIndex = static_cast<std::size_t>(bitIndex / 8);
            const auto shift = 7 - (bitIndex % 8);
            value = static_cast<std::uint8_t>((value << 1U) | ((random[byteIndex] >> shift) & 0x01U));
            ++bitIndex;
        }
        out[static_cast<std::size_t>(i)] = kCrockfordBase32[value];
    }

    return out;
}

[[nodiscard]] std::shared_ptr<IRandomSource> defaultRandomSource()
{
    static auto source = std::make_shared<OsRandomSource>();
    return source;
}

} // namespace

Status validateIdGeneratorOptions(const IdGeneratorOptions& options)
{
    switch (options.strategy_) {
    case IdStrategy::UuidV4:
    case IdStrategy::Ulid:
    case IdStrategy::Monotonic:
        break;
    default:
        return Status::invalidArgument("unknown id generation strategy");
    }

    if (auto status = validateText(options.prefix_, "prefix", options.maxPrefixLength_, kAllowedIdText); !status.isOk())
        return status;
    if (auto status = validateText(options.separator_, "separator", options.maxSeparatorLength_, kAllowedSeparatorText); !status.isOk())
        return status;

    return Status::ok();
}

Result<void> OsRandomSource::fill(std::span<std::byte> data)
{
    return fillRandomFromOs(data);
}

std::shared_ptr<IIdClock> SystemIdClock::instance()
{
    static auto clock = std::shared_ptr<IIdClock>(new SystemIdClock());
    return clock;
}

Result<std::chrono::milliseconds::rep> SystemIdClock::unixTimeMs() const
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return std::max<std::chrono::milliseconds::rep>(now, 0);
}

IdGenerator::IdGenerator(IdGeneratorOptions options)
    : options_(std::move(options))
    , status_(validateIdGeneratorOptions(options_))
    , randomSource_(options_.randomSource_ ? options_.randomSource_ : defaultRandomSource())
    , clock_(options_.clock_ ? options_.clock_ : SystemIdClock::instance())
    , counter_(options_.monotonicStart_)
{
}

IdGenerator IdGenerator::uuidV4(std::string prefix)
{
    IdGeneratorOptions options;
    options.strategy_ = IdStrategy::UuidV4;
    options.prefix_ = std::move(prefix);
    return IdGenerator(std::move(options));
}

IdGenerator IdGenerator::ulid(std::string prefix)
{
    IdGeneratorOptions options;
    options.strategy_ = IdStrategy::Ulid;
    options.prefix_ = std::move(prefix);
    return IdGenerator(std::move(options));
}

IdGenerator IdGenerator::monotonic(std::string prefix, std::uint64_t start)
{
    IdGeneratorOptions options;
    options.strategy_ = IdStrategy::Monotonic;
    options.prefix_ = std::move(prefix);
    options.monotonicStart_ = start;
    return IdGenerator(std::move(options));
}

Result<std::string> IdGenerator::next()
{
    return next(options_.prefix_);
}

Result<std::string> IdGenerator::next(std::string_view prefixOverride)
{
    if (!status_.isOk())
        return status_;
    if (auto status = validatePrefix(prefixOverride); !status.isOk())
        return status;

    auto raw = nextRaw();
    if (!raw.isOk())
        return raw.status();
    return withPrefix(prefixOverride, std::move(*raw));
}

IdStrategy IdGenerator::strategy() const noexcept
{
    return options_.strategy_;
}

const std::string& IdGenerator::prefix() const noexcept
{
    return options_.prefix_;
}

const std::string& IdGenerator::separator() const noexcept
{
    return options_.separator_;
}

const Status& IdGenerator::status() const noexcept
{
    return status_;
}

Status IdGenerator::validatePrefix(std::string_view prefix) const
{
    return validateText(prefix, "prefix", options_.maxPrefixLength_, kAllowedIdText);
}

Result<std::string> IdGenerator::nextRaw()
{
    switch (options_.strategy_) {
    case IdStrategy::UuidV4:
        return nextUuidV4();
    case IdStrategy::Ulid:
        return nextUlid();
    case IdStrategy::Monotonic:
        return nextMonotonic();
    }
    return Status::invalidArgument("unknown id generation strategy");
}

Result<std::string> IdGenerator::withPrefix(std::string_view prefix, std::string raw) const
{
    if (prefix.empty())
        return raw;

    std::string out;
    out.reserve(prefix.size() + options_.separator_.size() + raw.size());
    out.append(prefix);
    out.append(options_.separator_);
    out.append(raw);
    return out;
}

Result<std::string> IdGenerator::nextUuidV4()
{
    std::array<std::byte, 16> bytes {};
    auto filled = randomSource_->fill(bytes);
    if (!filled.isOk())
        return filled.status();

    bytes[6] = static_cast<std::byte>((std::to_integer<unsigned char>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::byte>((std::to_integer<unsigned char>(bytes[8]) & 0x3FU) | 0x80U);

    std::string out(36, '0');
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[cursor++] = '-';

        const auto value = std::to_integer<unsigned char>(bytes[i]);
        out[cursor++] = kHexAlphabet[(value >> 4U) & 0x0FU];
        out[cursor++] = kHexAlphabet[value & 0x0FU];
    }

    return out;
}

Result<std::string> IdGenerator::nextUlid()
{
    std::lock_guard lock(ulidMutex_);

    auto nowResult = clock_->unixTimeMs();
    if (!nowResult.isOk())
        return nowResult.status();

    auto now = *nowResult;
    if (auto status = validateUlidTimeMs(now); !status.isOk())
        return status;

    if (now > lastUlidTimeMs_) {
        lastUlidTimeMs_ = now;
        auto randomBytes = std::as_writable_bytes(std::span(lastUlidRandom_, sizeof(lastUlidRandom_)));
        auto filled = randomSource_->fill(randomBytes);
        if (!filled.isOk())
            return filled.status();
        return encodeUlid(lastUlidTimeMs_, lastUlidRandom_);
    }

    if (!incrementBigEndian(lastUlidRandom_, sizeof(lastUlidRandom_)))
        return Status::resourceExhausted("ULID monotonic random space exhausted for current millisecond");

    return encodeUlid(lastUlidTimeMs_, lastUlidRandom_);
}

Result<std::string> IdGenerator::nextMonotonic()
{
    auto current = counter_.load(std::memory_order_relaxed);
    for (;;) {
        if (current == std::numeric_limits<std::uint64_t>::max())
            return Status::outOfRange("monotonic id counter exhausted");

        if (counter_.compare_exchange_weak(
                current,
                current + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return std::to_string(current);
        }
    }
}

} // namespace lgc
