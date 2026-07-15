#include "foundation/blob/blob_common.hh"

#include "foundation/crypto/crypto.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>

#include <openssl/evp.h>

namespace lgc::blob_detail {
namespace {

class Sha256Context final {
public:
    Sha256Context() noexcept
        : context_(EVP_MD_CTX_new())
    {
    }

    Sha256Context(const Sha256Context&) = delete;
    Sha256Context& operator=(const Sha256Context&) = delete;

    ~Sha256Context()
    {
        EVP_MD_CTX_free(context_);
    }

    [[nodiscard]] Result<void> init()
    {
        if (context_ == nullptr)
            return Status::resourceExhausted("failed to allocate blob hash context");
        if (EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1)
            return Status::internal("failed to initialize blob hash context");
        return okResult();
    }

    [[nodiscard]] Result<void> update(std::span<const std::byte> bytes)
    {
        if (!bytes.empty() && EVP_DigestUpdate(context_, bytes.data(), bytes.size()) != 1)
            return Status::internal("failed to update blob hash");
        return okResult();
    }

    [[nodiscard]] Result<std::string> finish()
    {
        Bytes digest(static_cast<std::size_t>(EVP_MD_get_size(EVP_sha256())));
        unsigned int digestSize = 0;
        if (EVP_DigestFinal_ex(context_, digest.data(), &digestSize) != 1)
            return Status::internal("failed to finalize blob hash");
        digest.resize(digestSize);
        return toHex(digest);
    }

private:
    EVP_MD_CTX* context_ { nullptr };
};

[[nodiscard]] bool isHex(std::string_view value) noexcept
{
    for (const auto ch : value) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'f';
        const bool upper = ch >= 'A' && ch <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }
    return true;
}

} // namespace

bool matches(const BlobKey& key, const BlobListOptions& options)
{
    if (!options.namespace_.empty() && key.namespace_ != options.namespace_)
        return false;
    if (!options.namePrefix_.empty() && !key.name_.starts_with(options.namePrefix_))
        return false;
    return true;
}

std::pair<std::string, std::string> toOrderedKey(const BlobKey& key)
{
    return { key.namespace_, key.name_ };
}

fs::path lockPath(const fs::path& rootDirectory)
{
    return rootDirectory / kBlobStoreLockName;
}

Status validateStoreMarkerFile(const fs::path& marker)
{
    auto markerContents = readFile(marker, ReadFileOptions { .maxBytes_ = kBlobStoreMarkerContents.size() + 1 });
    if (!markerContents.isOk())
        return markerContents.status();
    if (*markerContents != kBlobStoreMarkerContents)
        return Status::failedPrecondition("blob store marker contents are invalid");
    return Status::ok();
}

bool isSha256Hex(std::string_view value) noexcept
{
    return value.size() == kSha256HexLength && isHex(value);
}

Result<std::string> sha256Hex(std::span<const std::byte> data)
{
    return digestHex(
        HashAlgorithm::Sha256,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(data.data()),
            data.size()));
}

Status requireBlobWithinLimit(std::size_t size, const BlobStoreOptions& options)
{
    if (options.maxBlobBytes_ && size > *options.maxBlobBytes_)
        return Status::resourceExhausted("blob exceeds configured maximum size");
    return Status::ok();
}

Status validateBlobListOptionsForStore(
    const BlobListOptions& listOptions,
    const BlobStoreOptions& storeOptions)
{
    if (auto status = validateBlobListOptions(listOptions); !status.isOk())
        return status;
    if (storeOptions.maxListItems_ != 0U && listOptions.limit_ > storeOptions.maxListItems_)
        return Status::resourceExhausted("blob list limit exceeds configured maximum");
    return Status::ok();
}

Result<bool> initializedStore(const fs::path& rootDirectory)
{
    std::error_code ec;
    if (!fs::exists(rootDirectory, ec)) {
        if (ec)
            return Status::internal("failed to check blob store root: " + ec.message());
        return false;
    }
    if (!fs::is_directory(rootDirectory, ec) || ec)
        return Status::failedPrecondition("blob store root exists but is not a directory");

    const auto marker = rootDirectory / kBlobStoreMarkerName;
    if (!fs::exists(marker, ec)) {
        if (ec)
            return Status::internal("failed to check blob store marker: " + ec.message());
        return false;
    }
    if (!fs::is_regular_file(marker, ec) || ec)
        return Status::failedPrecondition("blob store marker path exists but is not a regular file");

    if (auto status = validateStoreMarkerFile(marker); !status.isOk())
        return status;
    return true;
}

std::chrono::system_clock::time_point nowSystem()
{
    return std::chrono::system_clock::now();
}

std::int64_t toUnixMillis(std::chrono::system_clock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromUnixMillis(std::int64_t value)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

bool isMetadataFile(const fs::path& path)
{
    return path.filename().string().ends_with(".meta.json");
}

bool isBlobTempFile(const fs::path& path)
{
    const auto name = path.filename().string();
    return (name.starts_with(".blob-") || name.starts_with(".tmp-")) && name.ends_with(".tmp");
}

bool isBlobContentFile(const fs::path& path)
{
    return path.extension() == ".data";
}

Result<BlobData> readBytes(const fs::path& path, const BlobStoreOptions& options)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return Status::notFound("blob data not found: " + path.string());

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0)
        return Status::internal("failed to determine blob size: " + path.string());
    if (static_cast<std::uintmax_t>(size) > std::numeric_limits<std::size_t>::max())
        return Status::resourceExhausted("blob is too large for this platform");
    const auto byteSize = static_cast<std::size_t>(size);
    if (auto status = requireBlobWithinLimit(byteSize, options); !status.isOk())
        return status;

    BlobData data(byteSize);
    if (!data.empty()) {
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (file.gcount() != static_cast<std::streamsize>(data.size()))
            return Status::dataLoss("blob data changed while reading");
    }
    if (!file && !file.eof())
        return Status::internal("failed to read blob data: " + path.string());
    return data;
}

Result<void> streamBytes(
    const fs::path& path,
    const BlobInfo& info,
    const BlobStoreOptions& storeOptions,
    const BlobReadCallback& callback,
    const BlobReadOptions& readOptions)
{
    if (!callback)
        return Status::invalidArgument("blob read callback cannot be empty");
    if (auto status = validateBlobReadOptions(readOptions); !status.isOk())
        return status;
    if (auto status = requireBlobWithinLimit(info.size_, storeOptions); !status.isOk())
        return status;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return Status::notFound("blob data not found: " + path.string());

    Sha256Context hash;
    if (auto result = hash.init(); !result.isOk())
        return result.status();

    BlobData buffer(readOptions.chunkBytes_);
    std::size_t total = 0;
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count <= 0)
            break;

        const auto chunk = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count));
        total += chunk.size();
        if (total > info.size_)
            return Status::dataLoss("blob data is larger than metadata");
        if (auto result = hash.update(chunk); !result.isOk())
            return result.status();
        if (auto status = callback(chunk); !status.isOk())
            return status;
    }

    if (file.bad())
        return Status::internal("failed to stream blob data: " + path.string());
    if (total != info.size_)
        return Status::dataLoss("blob data size does not match metadata");

    auto checksum = hash.finish();
    if (!checksum.isOk())
        return checksum.status();
    if (*checksum != info.checksumSha256_)
        return Status::dataLoss("blob checksum mismatch");
    return okResult();
}

Result<StagedContent> stageContentFromFile(
    const fs::path& sourcePath,
    const fs::path& stagingDirectory,
    const BlobStoreOptions& options)
{
    std::error_code ec;
    if (fs::is_directory(sourcePath, ec) && !ec)
        return Status::failedPrecondition("blob source path is a directory");

    const auto sourceSize = fs::file_size(sourcePath, ec);
    if (!ec) {
        if (sourceSize > std::numeric_limits<std::size_t>::max())
            return Status::resourceExhausted("blob source file is too large for this platform");
        if (auto status = requireBlobWithinLimit(static_cast<std::size_t>(sourceSize), options); !status.isOk())
            return status;
    }

    std::ifstream source(sourcePath, std::ios::binary);
    if (!source)
        return Status::notFound("failed to open blob source file: " + sourcePath.string());

    auto temp = TempFile::create(TempFileOptions {
        .directory_ = stagingDirectory,
        .prefix_ = ".blob-",
        .suffix_ = ".tmp",
    });
    if (!temp.isOk())
        return temp.status();

    Sha256Context hash;
    if (auto result = hash.init(); !result.isOk())
        return result.status();

    BlobData buffer(64 * 1024);
    std::size_t total = 0;
    while (source) {
        source.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = source.gcount();
        if (count <= 0)
            break;

        const auto chunk = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count));
        total += chunk.size();
        if (auto status = requireBlobWithinLimit(total, options); !status.isOk())
            return status;
        if (auto result = hash.update(chunk); !result.isOk())
            return result.status();
        if (auto result = temp->write(chunk); !result.isOk())
            return result.status();
    }
    if (source.bad())
        return Status::internal("failed to read blob source file: " + sourcePath.string());

    if (auto result = temp->flush(); !result.isOk())
        return result.status();
    if (auto result = temp->close(); !result.isOk())
        return result.status();

    auto checksum = hash.finish();
    if (!checksum.isOk())
        return checksum.status();

    return StagedContent {
        .temp_ = std::move(*temp),
        .size_ = total,
        .checksumSha256_ = std::move(*checksum),
    };
}

Result<bool> installStagedContent(
    TempFile& temp,
    const fs::path& contentPath,
    std::size_t expectedSize)
{
    std::error_code ec;
    if (fs::exists(contentPath, ec)) {
        if (ec)
            return Status::internal("failed to check blob content: " + ec.message());
        const auto actualSize = fs::file_size(contentPath, ec);
        if (ec)
            return Status::dataLoss("blob content exists but is unreadable");
        if (actualSize != static_cast<std::uintmax_t>(expectedSize))
            return Status::dataLoss("blob content-addressed file has unexpected size");
        if (auto result = temp.remove(); !result.isOk())
            return result.status();
        return false;
    }
    if (ec)
        return Status::internal("failed to check blob content: " + ec.message());

    const auto parent = contentPath.parent_path();
    if (auto result = ensureDir(parent); !result.isOk())
        return result.status();

    const auto tempPath = temp.path();
    fs::rename(tempPath, contentPath, ec);
    if (ec)
        return Status::internal("failed to install staged blob content: " + ec.message());

    (void)temp.release();
    return true;
}

Result<void> writeDataFile(const fs::path& path, std::span<const std::byte> data)
{
    return writeFileAtomic(path, data, AtomicWriteOptions { .durable_ = true });
}

Status removeFileStrict(const fs::path& path, std::string_view label)
{
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        std::string message("failed to remove ");
        message.append(label);
        message.append(": ");
        message.append(ec.message());
        return Status::internal(std::move(message));
    }
    return Status::ok();
}

} // namespace lgc::blob_detail
