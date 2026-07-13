#pragma once

#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lc {

inline constexpr std::size_t kDefaultMaxDecompressedBytes = 64 * 1024 * 1024;

enum class CompressionAlgorithm : std::uint8_t {
    None = 0,
    Gzip,
    Zstd,
};

struct CompressionOptions {
    CompressionAlgorithm algorithm_ { CompressionAlgorithm::None };
    std::optional<int> level_;
    std::size_t maxOutputBytes_ { 0 };
};

struct DecompressionOptions {
    std::size_t maxOutputBytes_ { kDefaultMaxDecompressedBytes };
    bool allowTrailingBytes_ { false };
};

struct CompressedData {
    CompressionAlgorithm algorithm_ { CompressionAlgorithm::None };
    std::vector<std::byte> data_;
    std::optional<std::size_t> originalBytes_;
};

class ICompressor {
public:
    virtual ~ICompressor() = default;

    ICompressor(const ICompressor&) = delete;
    ICompressor& operator=(const ICompressor&) = delete;
    ICompressor(ICompressor&&) = delete;
    ICompressor& operator=(ICompressor&&) = delete;

protected:
    ICompressor() = default;

public:
    [[nodiscard]] virtual bool supports(CompressionAlgorithm algorithm) const noexcept = 0;

    [[nodiscard]] virtual Result<CompressedData> compress(
        std::span<const std::byte> input,
        const CompressionOptions& options = {}) const
        = 0;

    [[nodiscard]] virtual Result<std::vector<std::byte>> decompress(
        std::span<const std::byte> input,
        CompressionAlgorithm algorithm,
        const DecompressionOptions& options = {}) const
        = 0;

    [[nodiscard]] virtual Result<std::vector<std::byte>> decompress(
        const CompressedData& data,
        const DecompressionOptions& options = {}) const
        = 0;
};

class Compressor final : public ICompressor {
public:
    [[nodiscard]] bool supports(CompressionAlgorithm algorithm) const noexcept override;

    [[nodiscard]] Result<CompressedData> compress(
        std::span<const std::byte> input,
        const CompressionOptions& options = {}) const override;

    [[nodiscard]] Result<std::vector<std::byte>> decompress(
        std::span<const std::byte> input,
        CompressionAlgorithm algorithm,
        const DecompressionOptions& options = {}) const override;

    [[nodiscard]] Result<std::vector<std::byte>> decompress(
        const CompressedData& data,
        const DecompressionOptions& options = {}) const override;
};

[[nodiscard]] std::string_view compressionName(CompressionAlgorithm algorithm) noexcept;
[[nodiscard]] std::string_view compressionEncoding(CompressionAlgorithm algorithm) noexcept;

[[nodiscard]] Result<CompressedData> compressString(
    const ICompressor& compressor,
    std::string_view input,
    const CompressionOptions& options = {});

[[nodiscard]] Result<std::string> decompressString(
    const ICompressor& compressor,
    const CompressedData& data,
    const DecompressionOptions& options = {});

} // namespace lc
