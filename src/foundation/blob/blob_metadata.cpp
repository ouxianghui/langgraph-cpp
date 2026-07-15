#include "foundation/blob/blob_metadata.hh"

#include "foundation/blob/blob_common.hh"
#include "foundation/filesystem/filesystem.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace lgc::blob_detail {

[[nodiscard]] Status metadataError(std::string message)
{
    return Status::dataLoss("invalid blob metadata: " + std::move(message));
}

[[nodiscard]] bool isVisibleAscii(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](unsigned char ch) {
        return ch >= 0x21 && ch <= 0x7e;
    });
}

[[nodiscard]] Status validateContentType(std::string_view value, const BlobStoreOptions& options)
{
    if (value.empty())
        return Status::invalidArgument("blob content type cannot be empty");
    if (options.maxContentTypeBytes_ != 0U && value.size() > options.maxContentTypeBytes_)
        return Status::invalidArgument("blob content type is too long");
    if (!isVisibleAscii(value))
        return Status::invalidArgument("blob content type must contain visible ASCII only");

    const auto slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= value.size())
        return Status::invalidArgument("blob content type must be a media type");
    return Status::ok();
}

[[nodiscard]] Status validateMetadata(const BlobMetadata& metadata, const BlobStoreOptions& options)
{
    if (options.maxMetadataEntries_ != 0U && metadata.size() > options.maxMetadataEntries_)
        return Status::resourceExhausted("blob metadata has too many entries");

    for (const auto& [key, value] : metadata) {
        if (key.empty())
            return Status::invalidArgument("blob metadata key cannot be empty");
        if (options.maxMetadataKeyBytes_ != 0U && key.size() > options.maxMetadataKeyBytes_)
            return Status::invalidArgument("blob metadata key is too long");
        if (options.maxMetadataValueBytes_ != 0U && value.size() > options.maxMetadataValueBytes_)
            return Status::invalidArgument("blob metadata value is too long");
        for (const char ch : key) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x21 || byte > 0x7e)
                return Status::invalidArgument("blob metadata key must contain visible ASCII only");
        }
    }
    return Status::ok();
}

[[nodiscard]] Status validatePutOptions(const BlobPutOptions& putOptions, const BlobStoreOptions& storeOptions)
{
    if (auto status = validateContentType(putOptions.contentType_, storeOptions); !status.isOk())
        return status;
    return validateMetadata(putOptions.metadata_, storeOptions);
}
[[nodiscard]] Status metadataStatusFromValidation(const Status& status)
{
    if (status.isOk())
        return Status::ok();
    return metadataError(std::string(status.message()));
}
[[nodiscard]] bool isInfoField(std::string_view key) noexcept
{
    return key == "namespace" || key == "name" || key == "content_type" || key == "size"
        || key == "checksum_sha256" || key == "metadata" || key == "created_at_unix_ms"
        || key == "updated_at_unix_ms";
}

[[nodiscard]] Result<std::string> requiredStringField(const nlohmann::json& value, std::string_view name)
{
    const auto it = value.find(std::string(name));
    if (it == value.end())
        return metadataError("field '" + std::string(name) + "' is missing");
    if (!it->is_string())
        return metadataError("field '" + std::string(name) + "' must be a string");
    return it->get<std::string>();
}

[[nodiscard]] Result<std::string> requiredSha256Field(const nlohmann::json& value)
{
    auto checksum = requiredStringField(value, "checksum_sha256");
    if (!checksum.isOk())
        return checksum.status();
    if (!isSha256Hex(*checksum))
        return metadataError("field 'checksum_sha256' must be a SHA-256 hex digest");
    return checksum;
}

[[nodiscard]] Result<std::size_t> requiredSizeField(const nlohmann::json& value, std::string_view name)
{
    const auto it = value.find(std::string(name));
    if (it == value.end())
        return metadataError("field '" + std::string(name) + "' is missing");
    if (!it->is_number_integer() && !it->is_number_unsigned())
        return metadataError("field '" + std::string(name) + "' must be a non-negative integer");

    try {
        std::uint64_t unsignedValue = 0;
        if (it->is_number_unsigned()) {
            unsignedValue = it->get<std::uint64_t>();
        } else {
            const auto signedValue = it->get<std::int64_t>();
            if (signedValue < 0)
                return metadataError("field '" + std::string(name) + "' must be non-negative");
            unsignedValue = static_cast<std::uint64_t>(signedValue);
        }
        if (unsignedValue > std::numeric_limits<std::size_t>::max())
            return metadataError("field '" + std::string(name) + "' is too large");
        return static_cast<std::size_t>(unsignedValue);
    } catch (const std::exception& e) {
        return metadataError("field '" + std::string(name) + "' is invalid: " + e.what());
    }
}

[[nodiscard]] Result<std::int64_t> requiredInt64Field(const nlohmann::json& value, std::string_view name)
{
    const auto it = value.find(std::string(name));
    if (it == value.end())
        return metadataError("field '" + std::string(name) + "' is missing");
    if (!it->is_number_integer())
        return metadataError("field '" + std::string(name) + "' must be an integer");

    try {
        return it->get<std::int64_t>();
    } catch (const std::exception& e) {
        return metadataError("field '" + std::string(name) + "' is invalid: " + e.what());
    }
}

[[nodiscard]] Result<BlobMetadata> requiredMetadataField(const nlohmann::json& value, const BlobStoreOptions& options)
{
    const auto it = value.find("metadata");
    if (it == value.end())
        return metadataError("field 'metadata' is missing");
    if (!it->is_object())
        return metadataError("field 'metadata' must be an object");

    BlobMetadata metadata;
    for (const auto& item : it->items()) {
        if (!item.value().is_string())
            return metadataError("metadata value for '" + item.key() + "' must be a string");
        metadata.emplace(item.key(), item.value().get<std::string>());
    }
    if (auto status = validateMetadata(metadata, options); !status.isOk())
        return metadataStatusFromValidation(status);
    return metadata;
}
[[nodiscard]] nlohmann::json infoToJson(const BlobInfo& info)
{
    return nlohmann::json {
        { "namespace", info.key_.namespace_ },
        { "name", info.key_.name_ },
        { "content_type", info.contentType_ },
        { "size", info.size_ },
        { "checksum_sha256", info.checksumSha256_ },
        { "metadata", info.metadata_ },
        { "created_at_unix_ms", toUnixMillis(info.createdAt_) },
        { "updated_at_unix_ms", toUnixMillis(info.updatedAt_) },
    };
}

[[nodiscard]] Result<BlobInfo> infoFromJson(const nlohmann::json& value, const BlobStoreOptions& options)
{
    if (!value.is_object())
        return metadataError("root must be an object");

    for (const auto& item : value.items()) {
        if (!isInfoField(item.key()))
            return metadataError("unexpected field '" + item.key() + "'");
    }

    auto namespaceName = requiredStringField(value, "namespace");
    if (!namespaceName.isOk())
        return namespaceName.status();

    auto name = requiredStringField(value, "name");
    if (!name.isOk())
        return name.status();

    auto contentType = requiredStringField(value, "content_type");
    if (!contentType.isOk())
        return contentType.status();
    if (auto status = validateContentType(*contentType, options); !status.isOk())
        return metadataStatusFromValidation(status);

    auto size = requiredSizeField(value, "size");
    if (!size.isOk())
        return size.status();

    auto checksum = requiredSha256Field(value);
    if (!checksum.isOk())
        return checksum.status();

    auto metadata = requiredMetadataField(value, options);
    if (!metadata.isOk())
        return metadata.status();

    auto createdAt = requiredInt64Field(value, "created_at_unix_ms");
    if (!createdAt.isOk())
        return createdAt.status();

    auto updatedAt = requiredInt64Field(value, "updated_at_unix_ms");
    if (!updatedAt.isOk())
        return updatedAt.status();

    BlobInfo info {
        .key_ = BlobKey {
            .namespace_ = std::move(*namespaceName),
            .name_ = std::move(*name),
        },
        .contentType_ = std::move(*contentType),
        .size_ = *size,
        .checksumSha256_ = std::move(*checksum),
        .metadata_ = std::move(*metadata),
        .createdAt_ = fromUnixMillis(*createdAt),
        .updatedAt_ = fromUnixMillis(*updatedAt),
    };
    if (auto status = validateBlobKey(info.key_); !status.isOk())
        return Status::dataLoss(std::string(status.message()));
    if (info.createdAt_ > info.updatedAt_)
        return metadataError("created_at_unix_ms cannot be newer than updated_at_unix_ms");
    return info;
}

[[nodiscard]] Result<BlobInfo> readInfoFile(const fs::path& path, const BlobStoreOptions& options)
{
    auto text = readFile(path, ReadFileOptions { .maxBytes_ = options.maxMetadataBytes_ });
    if (!text.isOk())
        return text.status();
    try {
        return infoFromJson(nlohmann::json::parse(*text), options);
    } catch (const std::exception& e) {
        return Status::dataLoss(std::string("failed to parse blob metadata: ") + e.what());
    }
}

[[nodiscard]] Result<void> writeInfoFile(const fs::path& path, const BlobInfo& info)
{
    return writeFileAtomic(path, infoToJson(info).dump(), AtomicWriteOptions { .durable_ = true });
}


} // namespace lgc::blob_detail
