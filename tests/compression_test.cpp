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
    const lgc::Compressor compressor;
    const std::string text = "checkpoint-state-checkpoint-state-checkpoint-state";

    assert(compressor.supports(lgc::CompressionAlgorithm::None));
    assert(lgc::compressionName(lgc::CompressionAlgorithm::Gzip) == "gzip");
    assert(lgc::compressionEncoding(lgc::CompressionAlgorithm::None) == "identity");

    auto identity = lgc::compressString(
        compressor,
        text,
        lgc::CompressionOptions { .algorithm_ = lgc::CompressionAlgorithm::None });
    assert(identity.isOk());
    assert(identity->algorithm_ == lgc::CompressionAlgorithm::None);
    assert(identity->originalBytes_.has_value());
    assert(*identity->originalBytes_ == text.size());

    auto identityRoundTrip = lgc::decompressString(compressor, *identity);
    assert(identityRoundTrip.isOk());
    assert(*identityRoundTrip == text);

    auto tooLarge = compressor.decompress(
        identity->data_,
        identity->algorithm_,
        lgc::DecompressionOptions { .maxOutputBytes_ = 4 });
    assert(!tooLarge.isOk());
    assert(tooLarge.status().code() == lgc::StatusCode::ResourceExhausted);

    auto compressTooLarge = lgc::compressString(
        compressor,
        text,
        lgc::CompressionOptions {
            .algorithm_ = lgc::CompressionAlgorithm::None,
            .maxOutputBytes_ = 4,
        });
    assert(!compressTooLarge.isOk());
    assert(compressTooLarge.status().code() == lgc::StatusCode::ResourceExhausted);

    const std::vector<std::byte> emptyBytes;
    auto emptyIdentity = compressor.compress(
        emptyBytes,
        lgc::CompressionOptions { .algorithm_ = lgc::CompressionAlgorithm::None });
    assert(emptyIdentity.isOk());
    assert(emptyIdentity->originalBytes_.has_value());
    assert(*emptyIdentity->originalBytes_ == 0);
    auto emptyRoundTrip = compressor.decompress(*emptyIdentity);
    assert(emptyRoundTrip.isOk());
    assert(emptyRoundTrip->empty());

    auto noneMismatch = compressor.decompress(lgc::CompressedData {
        .algorithm_ = lgc::CompressionAlgorithm::None,
        .data_ = bytesOf(text),
        .originalBytes_ = 0,
    });
    assert(!noneMismatch.isOk());
    assert(noneMismatch.status().code() == lgc::StatusCode::DataLoss);

    auto noneWithoutExpectedSize = compressor.decompress(lgc::CompressedData {
        .algorithm_ = lgc::CompressionAlgorithm::None,
        .data_ = bytesOf(text),
    });
    assert(noneWithoutExpectedSize.isOk());
    assert(noneWithoutExpectedSize->size() == text.size());

    auto shared = std::make_shared<lgc::Compressor>();
    assert(shared->supports(lgc::CompressionAlgorithm::None));

    auto zstd = lgc::compressString(
        compressor,
        text,
        lgc::CompressionOptions { .algorithm_ = lgc::CompressionAlgorithm::Zstd });
    assert(!zstd.isOk());
    assert(zstd.status().code() == lgc::StatusCode::Unimplemented);

#if LANGGRAPH_CPP_HAS_ZLIB
    assert(compressor.supports(lgc::CompressionAlgorithm::Gzip));
    auto gzip = lgc::compressString(
        compressor,
        text,
        lgc::CompressionOptions {
            .algorithm_ = lgc::CompressionAlgorithm::Gzip,
            .level_ = 6,
        });
    assert(gzip.isOk());
    assert(gzip->algorithm_ == lgc::CompressionAlgorithm::Gzip);
    assert(gzip->originalBytes_.has_value());
    assert(*gzip->originalBytes_ == text.size());
    assert(!gzip->data_.empty());
    assert(gzip->data_.size() != text.size());

    auto gzipRoundTrip = lgc::decompressString(compressor, *gzip);
    assert(gzipRoundTrip.isOk());
    assert(*gzipRoundTrip == text);

    auto invalidLevel = lgc::compressString(
        compressor,
        text,
        lgc::CompressionOptions {
            .algorithm_ = lgc::CompressionAlgorithm::Gzip,
            .level_ = 99,
        });
    assert(!invalidLevel.isOk());
    assert(invalidLevel.status().code() == lgc::StatusCode::InvalidArgument);

    auto gzipTooLarge = compressor.decompress(
        *gzip,
        lgc::DecompressionOptions { .maxOutputBytes_ = text.size() - 1 });
    assert(!gzipTooLarge.isOk());
    assert(gzipTooLarge.status().code() == lgc::StatusCode::ResourceExhausted);

    auto invalidGzip = compressor.decompress(
        std::as_bytes(std::span(text.data(), text.size())),
        lgc::CompressionAlgorithm::Gzip);
    assert(!invalidGzip.isOk());
    assert(invalidGzip.status().code() == lgc::StatusCode::DataLoss);

    auto truncated = *gzip;
    truncated.data_.pop_back();
    auto truncatedResult = compressor.decompress(truncated);
    assert(!truncatedResult.isOk());
    assert(truncatedResult.status().code() == lgc::StatusCode::DataLoss);

    auto corrupted = *gzip;
    corrupted.data_.back() = std::byte { static_cast<unsigned char>(
        std::to_integer<unsigned char>(corrupted.data_.back()) ^ 0xFFU) };
    auto corruptedResult = compressor.decompress(corrupted);
    assert(!corruptedResult.isOk());
    assert(corruptedResult.status().code() == lgc::StatusCode::DataLoss);

    auto trailing = *gzip;
    trailing.data_.push_back(std::byte { 0x00 });
    auto trailingResult = compressor.decompress(trailing);
    assert(!trailingResult.isOk());
    assert(trailingResult.status().code() == lgc::StatusCode::DataLoss);

    auto trailingAllowed = compressor.decompress(
        trailing,
        lgc::DecompressionOptions {
            .allowTrailingBytes_ = true,
        });
    assert(trailingAllowed.isOk());
    assert(trailingAllowed->size() == text.size());

    auto mismatch = *gzip;
    mismatch.originalBytes_ = text.size() + 1;
    auto mismatchResult = compressor.decompress(mismatch);
    assert(!mismatchResult.isOk());
    assert(mismatchResult.status().code() == lgc::StatusCode::DataLoss);

    std::string large(256 * 1024, 'x');
    for (std::size_t i = 0; i < large.size(); i += 251)
        large[i] = static_cast<char>('a' + (i % 26));
    auto largeGzip = lgc::compressString(
        compressor,
        large,
        lgc::CompressionOptions {
            .algorithm_ = lgc::CompressionAlgorithm::Gzip,
        });
    assert(largeGzip.isOk());
    auto largeRoundTrip = lgc::decompressString(compressor, *largeGzip);
    assert(largeRoundTrip.isOk());
    assert(*largeRoundTrip == large);
#else
    assert(!compressor.supports(lgc::CompressionAlgorithm::Gzip));
    auto gzip = lgc::compressString(
        compressor,
        text,
        lgc::CompressionOptions { .algorithm_ = lgc::CompressionAlgorithm::Gzip });
    assert(!gzip.isOk());
    assert(gzip.status().code() == lgc::StatusCode::Unimplemented);
#endif

    return 0;
}
