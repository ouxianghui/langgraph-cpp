#include "foundation/serialization/state_codec.hpp"

#include "foundation/serialization/state_codec_common.hh"

namespace lgc {
namespace {
using namespace state_codec_detail;
}

bool isJsonPayload(std::string_view contentType) noexcept
{
    return contentType.empty() || contentType == kJsonContentType;
}

bool isCheckpointPayload(std::string_view contentType) noexcept
{
    return contentType == kCheckpointJsonContentType;
}

bool isCheckpointWritePayload(std::string_view contentType) noexcept
{
    return contentType == kCheckpointWriteJsonContentType;
}

} // namespace lgc
