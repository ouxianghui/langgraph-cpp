#include "foundation/compression/compressor.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#if LANGGRAPH_CPP_HAS_ZLIB
#include <zlib.h>
#endif

namespace lc {
namespace {

[[nodiscard]] std::vector<std::byte> copyBytes(std::span<const std::byte> input)
{
    return std::vector<std::byte>(input.begin(), input.end());
}

[[nodiscard]] Status validateMaxOutput(std::size_t size, std::size_t maxOutputBytes)
{
    if (maxOutputBytes != 0U && size > maxOutputBytes)
        return Status::resourceExhausted("output exceeds maxOutputBytes");
    return Status::ok();
}

[[nodiscard]] Status validateExpectedOutput(std::size_t actual, std::optional<std::size_t> expected)
{
    if (expected.has_value() && actual != *expected)
        return Status::dataLoss("decompressed size does not match payload originalBytes");
    return Status::ok();
}

[[nodiscard]] Status validateExpectedLimit(
    std::optional<std::size_t> expected,
    const DecompressionOptions& options)
{
    if (expected.has_value() && options.maxOutputBytes_ != 0U && *expected > options.maxOutputBytes_)
        return Status::resourceExhausted("decompressed output exceeds maxOutputBytes");
    return Status::ok();
}

[[nodiscard]] Status unsupported(CompressionAlgorithm algorithm)
{
    std::string message("compression algorithm is not supported: ");
    message.append(compressionName(algorithm));
    return Status::unimplemented(std::move(message));
}

#if LANGGRAPH_CPP_HAS_ZLIB

constexpr std::size_t kChunkSize = 16 * 1024;

class ZStreamGuard final {
public:
    using EndFn = int (*)(z_streamp);

    ZStreamGuard(z_stream& stream, EndFn endFn) noexcept
        : stream_(&stream)
        , endFn_(endFn)
    {
    }

    ZStreamGuard(const ZStreamGuard&) = delete;
    ZStreamGuard& operator=(const ZStreamGuard&) = delete;

    ~ZStreamGuard()
    {
        if (active_)
            (void)endFn_(stream_);
    }

    void activate() noexcept { active_ = true; }

private:
    z_stream* stream_ { nullptr };
    EndFn endFn_ { nullptr };
    bool active_ { false };
};

[[nodiscard]] int gzipLevel(const CompressionOptions& options)
{
    if (!options.level_.has_value())
        return Z_DEFAULT_COMPRESSION;
    return *options.level_;
}

[[nodiscard]] Status validateGzipLevel(const CompressionOptions& options)
{
    if (!options.level_.has_value())
        return Status::ok();
    if (*options.level_ < 0 || *options.level_ > 9)
        return Status::invalidArgument("gzip compression level must be between 0 and 9");
    return Status::ok();
}

[[nodiscard]] Result<std::vector<std::byte>> gzipCompress(
    std::span<const std::byte> input,
    const CompressionOptions& options)
{
    if (auto status = validateGzipLevel(options); !status.isOk())
        return status;

    z_stream stream {};
    int rc = deflateInit2(
        &stream,
        gzipLevel(options),
        Z_DEFLATED,
        15 + 16,
        8,
        Z_DEFAULT_STRATEGY);
    if (rc != Z_OK)
        return Status::internal("failed to initialize gzip compressor");
    ZStreamGuard guard(stream, deflateEnd);
    guard.activate();

    std::vector<std::byte> output;
    std::size_t consumed = 0;

    do {
        if (stream.avail_in == 0 && consumed < input.size()) {
            const auto remaining = input.size() - consumed;
            const auto chunk = std::min<std::size_t>(remaining, std::numeric_limits<uInt>::max());
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data() + consumed));
            stream.avail_in = static_cast<uInt>(chunk);
            consumed += chunk;
        }

        const int flush = (consumed >= input.size()) ? Z_FINISH : Z_NO_FLUSH;

        do {
            const auto oldSize = output.size();
            output.resize(oldSize + kChunkSize);
            stream.next_out = reinterpret_cast<Bytef*>(output.data() + oldSize);
            stream.avail_out = static_cast<uInt>(kChunkSize);

            rc = deflate(&stream, flush);
            if (rc == Z_STREAM_ERROR)
                return Status::internal("gzip compression stream error");

            output.resize(oldSize + (kChunkSize - stream.avail_out));
            if (auto status = validateMaxOutput(output.size(), options.maxOutputBytes_); !status.isOk())
                return status;
        } while (stream.avail_out == 0);
    } while (rc != Z_STREAM_END);

    return output;
}

[[nodiscard]] Result<std::vector<std::byte>> gzipDecompress(
    std::span<const std::byte> input,
    const DecompressionOptions& options)
{
    z_stream stream {};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(std::min<std::size_t>(input.size(), std::numeric_limits<uInt>::max()));

    int rc = inflateInit2(&stream, 15 + 16);
    if (rc != Z_OK)
        return Status::internal("failed to initialize gzip decompressor");
    ZStreamGuard guard(stream, inflateEnd);
    guard.activate();

    std::vector<std::byte> output;
    std::size_t consumed = stream.avail_in;

    for (;;) {
        const auto oldSize = output.size();
        output.resize(oldSize + kChunkSize);
        stream.next_out = reinterpret_cast<Bytef*>(output.data() + oldSize);
        stream.avail_out = static_cast<uInt>(kChunkSize);

        rc = inflate(&stream, Z_NO_FLUSH);
        output.resize(oldSize + (kChunkSize - stream.avail_out));

        if (auto status = validateMaxOutput(output.size(), options.maxOutputBytes_); !status.isOk())
            return status;

        if (rc == Z_STREAM_END) {
            const auto consumedInputBytes = consumed - stream.avail_in;
            if (!options.allowTrailingBytes_ && consumedInputBytes != input.size())
                return Status::dataLoss("gzip payload contains trailing bytes");
            break;
        }

        if (rc != Z_OK) {
            if (rc == Z_DATA_ERROR)
                return Status::dataLoss("invalid gzip payload");
            if (rc == Z_BUF_ERROR)
                return Status::dataLoss("truncated gzip payload");
            return Status::invalidArgument("invalid gzip payload");
        }

        if (stream.avail_in == 0 && consumed < input.size()) {
            const auto remaining = input.size() - consumed;
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data() + consumed));
            stream.avail_in = static_cast<uInt>(std::min<std::size_t>(remaining, std::numeric_limits<uInt>::max()));
            consumed += stream.avail_in;
        } else if (stream.avail_in == 0 && stream.avail_out != 0) {
            return Status::dataLoss("truncated gzip payload");
        }
    }

    return output;
}

#endif

} // namespace

bool Compressor::supports(CompressionAlgorithm algorithm) const noexcept
{
    switch (algorithm) {
    case CompressionAlgorithm::None:
        return true;
    case CompressionAlgorithm::Gzip:
#if LANGGRAPH_CPP_HAS_ZLIB
        return true;
#else
        return false;
#endif
    case CompressionAlgorithm::Zstd:
        return false;
    }
    return false;
}

Result<CompressedData> Compressor::compress(
    std::span<const std::byte> input,
    const CompressionOptions& options) const
{
    if (!supports(options.algorithm_))
        return unsupported(options.algorithm_);

    if (options.algorithm_ == CompressionAlgorithm::None) {
        if (auto status = validateMaxOutput(input.size(), options.maxOutputBytes_); !status.isOk())
            return status;
        return CompressedData {
            .algorithm_ = CompressionAlgorithm::None,
            .data_ = copyBytes(input),
            .originalBytes_ = input.size(),
        };
    }

    if (options.algorithm_ == CompressionAlgorithm::Gzip) {
#if LANGGRAPH_CPP_HAS_ZLIB
        auto compressed = gzipCompress(input, options);
        if (!compressed.isOk())
            return compressed.status();
        return CompressedData {
            .algorithm_ = CompressionAlgorithm::Gzip,
            .data_ = std::move(*compressed),
            .originalBytes_ = input.size(),
        };
#else
        return unsupported(options.algorithm_);
#endif
    }

    return unsupported(options.algorithm_);
}

Result<std::vector<std::byte>> Compressor::decompress(
    std::span<const std::byte> input,
    CompressionAlgorithm algorithm,
    const DecompressionOptions& options) const
{
    if (!supports(algorithm))
        return unsupported(algorithm);

    if (algorithm == CompressionAlgorithm::None) {
        if (auto status = validateMaxOutput(input.size(), options.maxOutputBytes_); !status.isOk())
            return status;
        return copyBytes(input);
    }

    if (algorithm == CompressionAlgorithm::Gzip) {
#if LANGGRAPH_CPP_HAS_ZLIB
        return gzipDecompress(input, options);
#else
        return unsupported(algorithm);
#endif
    }

    return unsupported(algorithm);
}

Result<std::vector<std::byte>> Compressor::decompress(
    const CompressedData& payload,
    const DecompressionOptions& options) const
{
    if (auto status = validateExpectedLimit(payload.originalBytes_, options); !status.isOk())
        return status;

    auto decompressed = decompress(payload.data_, payload.algorithm_, options);
    if (!decompressed.isOk())
        return decompressed.status();
    if (auto status = validateExpectedOutput(decompressed->size(), payload.originalBytes_); !status.isOk())
        return status;
    return decompressed;
}

std::string_view compressionName(CompressionAlgorithm algorithm) noexcept
{
    switch (algorithm) {
    case CompressionAlgorithm::None:
        return "none";
    case CompressionAlgorithm::Gzip:
        return "gzip";
    case CompressionAlgorithm::Zstd:
        return "zstd";
    }
    return "unknown";
}

std::string_view compressionEncoding(CompressionAlgorithm algorithm) noexcept
{
    switch (algorithm) {
    case CompressionAlgorithm::None:
        return "identity";
    case CompressionAlgorithm::Gzip:
        return "gzip";
    case CompressionAlgorithm::Zstd:
        return "zstd";
    }
    return "identity";
}

Result<CompressedData> compressString(
    const ICompressor& compressor,
    std::string_view input,
    const CompressionOptions& options)
{
    return compressor.compress(std::as_bytes(std::span(input.data(), input.size())), options);
}

Result<std::string> decompressString(
    const ICompressor& compressor,
    const CompressedData& payload,
    const DecompressionOptions& options)
{
    auto bytes = compressor.decompress(payload, options);
    if (!bytes.isOk())
        return bytes.status();
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

} // namespace lc
