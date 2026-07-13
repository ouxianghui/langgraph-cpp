#include "foundation/compression/compressor.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

} // namespace

int main()
{
    const lc::Compressor compressor;
    const std::string text = "checkpoint-state-checkpoint-state-checkpoint-state";

    assert(compressor.supports(lc::CompressionAlgorithm::None));
    assert(lc::compressionName(lc::CompressionAlgorithm::Gzip) == "gzip");
    assert(lc::compressionEncoding(lc::CompressionAlgorithm::None) == "identity");

    auto identity = lc::compressString(
        compressor,
        text,
        lc::CompressionOptions { .algorithm_ = lc::CompressionAlgorithm::None });
    assert(identity.isOk());
    assert(identity->algorithm_ == lc::CompressionAlgorithm::None);
    assert(identity->originalBytes_.has_value());
    assert(*identity->originalBytes_ == text.size());

    auto identityRoundTrip = lc::decompressString(compressor, *identity);
    assert(identityRoundTrip.isOk());
    assert(*identityRoundTrip == text);

    auto tooLarge = compressor.decompress(
        identity->data_,
        identity->algorithm_,
        lc::DecompressionOptions { .maxOutputBytes_ = 4 });
    assert(!tooLarge.isOk());
    assert(tooLarge.status().code() == lc::StatusCode::ResourceExhausted);

    auto compressTooLarge = lc::compressString(
        compressor,
        text,
        lc::CompressionOptions {
            .algorithm_ = lc::CompressionAlgorithm::None,
            .maxOutputBytes_ = 4,
        });
    assert(!compressTooLarge.isOk());
    assert(compressTooLarge.status().code() == lc::StatusCode::ResourceExhausted);

    const std::vector<std::byte> emptyBytes;
    auto emptyIdentity = compressor.compress(
        emptyBytes,
        lc::CompressionOptions { .algorithm_ = lc::CompressionAlgorithm::None });
    assert(emptyIdentity.isOk());
    assert(emptyIdentity->originalBytes_.has_value());
    assert(*emptyIdentity->originalBytes_ == 0);
    auto emptyRoundTrip = compressor.decompress(*emptyIdentity);
    assert(emptyRoundTrip.isOk());
    assert(emptyRoundTrip->empty());

    auto noneMismatch = compressor.decompress(lc::CompressedData {
        .algorithm_ = lc::CompressionAlgorithm::None,
        .data_ = bytesOf(text),
        .originalBytes_ = 0,
    });
    assert(!noneMismatch.isOk());
    assert(noneMismatch.status().code() == lc::StatusCode::DataLoss);

    auto noneWithoutExpectedSize = compressor.decompress(lc::CompressedData {
        .algorithm_ = lc::CompressionAlgorithm::None,
        .data_ = bytesOf(text),
    });
    assert(noneWithoutExpectedSize.isOk());
    assert(noneWithoutExpectedSize->size() == text.size());

    auto shared = std::make_shared<lc::Compressor>();
    assert(shared->supports(lc::CompressionAlgorithm::None));

    auto zstd = lc::compressString(
        compressor,
        text,
        lc::CompressionOptions { .algorithm_ = lc::CompressionAlgorithm::Zstd });
    assert(!zstd.isOk());
    assert(zstd.status().code() == lc::StatusCode::Unimplemented);

#if LANGGRAPH_CPP_HAS_ZLIB
    assert(compressor.supports(lc::CompressionAlgorithm::Gzip));
    auto gzip = lc::compressString(
        compressor,
        text,
        lc::CompressionOptions {
            .algorithm_ = lc::CompressionAlgorithm::Gzip,
            .level_ = 6,
        });
    assert(gzip.isOk());
    assert(gzip->algorithm_ == lc::CompressionAlgorithm::Gzip);
    assert(gzip->originalBytes_.has_value());
    assert(*gzip->originalBytes_ == text.size());
    assert(!gzip->data_.empty());
    assert(gzip->data_.size() != text.size());

    auto gzipRoundTrip = lc::decompressString(compressor, *gzip);
    assert(gzipRoundTrip.isOk());
    assert(*gzipRoundTrip == text);

    auto invalidLevel = lc::compressString(
        compressor,
        text,
        lc::CompressionOptions {
            .algorithm_ = lc::CompressionAlgorithm::Gzip,
            .level_ = 99,
        });
    assert(!invalidLevel.isOk());
    assert(invalidLevel.status().code() == lc::StatusCode::InvalidArgument);

    auto gzipTooLarge = compressor.decompress(
        *gzip,
        lc::DecompressionOptions { .maxOutputBytes_ = text.size() - 1 });
    assert(!gzipTooLarge.isOk());
    assert(gzipTooLarge.status().code() == lc::StatusCode::ResourceExhausted);

    auto invalidGzip = compressor.decompress(
        std::as_bytes(std::span(text.data(), text.size())),
        lc::CompressionAlgorithm::Gzip);
    assert(!invalidGzip.isOk());
    assert(invalidGzip.status().code() == lc::StatusCode::DataLoss);

    auto truncated = *gzip;
    truncated.data_.pop_back();
    auto truncatedResult = compressor.decompress(truncated);
    assert(!truncatedResult.isOk());
    assert(truncatedResult.status().code() == lc::StatusCode::DataLoss);

    auto corrupted = *gzip;
    corrupted.data_.back() = std::byte { static_cast<unsigned char>(
        std::to_integer<unsigned char>(corrupted.data_.back()) ^ 0xFFU) };
    auto corruptedResult = compressor.decompress(corrupted);
    assert(!corruptedResult.isOk());
    assert(corruptedResult.status().code() == lc::StatusCode::DataLoss);

    auto trailing = *gzip;
    trailing.data_.push_back(std::byte { 0x00 });
    auto trailingResult = compressor.decompress(trailing);
    assert(!trailingResult.isOk());
    assert(trailingResult.status().code() == lc::StatusCode::DataLoss);

    auto trailingAllowed = compressor.decompress(
        trailing,
        lc::DecompressionOptions {
            .allowTrailingBytes_ = true,
        });
    assert(trailingAllowed.isOk());
    assert(trailingAllowed->size() == text.size());

    auto mismatch = *gzip;
    mismatch.originalBytes_ = text.size() + 1;
    auto mismatchResult = compressor.decompress(mismatch);
    assert(!mismatchResult.isOk());
    assert(mismatchResult.status().code() == lc::StatusCode::DataLoss);

    std::string large(256 * 1024, 'x');
    for (std::size_t i = 0; i < large.size(); i += 251)
        large[i] = static_cast<char>('a' + (i % 26));
    auto largeGzip = lc::compressString(
        compressor,
        large,
        lc::CompressionOptions {
            .algorithm_ = lc::CompressionAlgorithm::Gzip,
        });
    assert(largeGzip.isOk());
    auto largeRoundTrip = lc::decompressString(compressor, *largeGzip);
    assert(largeRoundTrip.isOk());
    assert(*largeRoundTrip == large);
#else
    assert(!compressor.supports(lc::CompressionAlgorithm::Gzip));
    auto gzip = lc::compressString(
        compressor,
        text,
        lc::CompressionOptions { .algorithm_ = lc::CompressionAlgorithm::Gzip });
    assert(!gzip.isOk());
    assert(gzip.status().code() == lc::StatusCode::Unimplemented);
#endif

    return 0;
}
