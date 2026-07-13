#include "foundation/serialization/state_codec.hpp"

#include "foundation/serialization/state_codec_common.hh"

#include <string>
#include <utility>

namespace lc {
namespace {
using state_codec_detail::kJsonContentType;
using state_codec_detail::requireJsonPayload;
}

JsonStateCodec::JsonStateCodec(JsonDecodeLimits limits)
    : limits_(std::move(limits))
{
}

Result<Payload> JsonStateCodec::encode(const State& state) const
{
    return Payload {
        .contentType_ = std::string(kJsonContentType),
        .data_ = state.json(),
    };
}

Result<State> JsonStateCodec::decode(const Payload& payload) const
{
    if (auto result = requireJsonPayload(payload); !result.isOk())
        return result.status();
    return State::fromJson(payload.data_, limits_);
}

} // namespace lc
