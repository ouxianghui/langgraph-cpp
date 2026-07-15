#pragma once

#include "foundation/blob/blob_store.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lgc::blob_detail {

[[nodiscard]] Status metadataError(std::string message);
[[nodiscard]] Status validateContentType(std::string_view value, const BlobStoreOptions& options);
[[nodiscard]] Status validateMetadata(const BlobMetadata& metadata, const BlobStoreOptions& options);
[[nodiscard]] Status validatePutOptions(const BlobPutOptions& putOptions, const BlobStoreOptions& storeOptions);
[[nodiscard]] Status metadataStatusFromValidation(const Status& status);
[[nodiscard]] nlohmann::json infoToJson(const BlobInfo& info);
[[nodiscard]] Result<BlobInfo> infoFromJson(const nlohmann::json& value, const BlobStoreOptions& options);
[[nodiscard]] Result<BlobInfo> readInfoFile(const std::filesystem::path& path, const BlobStoreOptions& options);
[[nodiscard]] Result<void> writeInfoFile(const std::filesystem::path& path, const BlobInfo& info);

} // namespace lgc::blob_detail
