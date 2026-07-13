#include "foundation/serialization/content_envelope.hpp"

#include <stdexcept>
#include <utility>

namespace lc {

EnvelopedStateCodec::EnvelopedStateCodec(
    std::shared_ptr<IStateCodec> inner,
    EnvelopeCodec envelope,
    EnvelopeOptions options)
    : inner_(std::move(inner))
    , envelope_(std::move(envelope))
    , options_(std::move(options))
{
    if (!inner_)
        throw std::invalid_argument("EnvelopedStateCodec requires an inner codec");
}

Result<Payload> EnvelopedStateCodec::encode(const State& state) const
{
    auto encoded = inner_->encode(state);
    if (!encoded.isOk())
        return encoded.status();
    return envelope_.encode(*encoded, options_);
}

Result<State> EnvelopedStateCodec::decode(const Payload& payload) const
{
    auto decoded = envelope_.decode(payload, options_);
    if (!decoded.isOk())
        return decoded.status();
    return inner_->decode(*decoded);
}

} // namespace lc
