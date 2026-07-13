#include "foundation/serialization/content_envelope.hpp"

#include <stdexcept>
#include <utility>

namespace lc {

EnvelopedCheckpointCodec::EnvelopedCheckpointCodec(
    std::shared_ptr<ICheckpointCodec> inner,
    EnvelopeCodec envelope,
    EnvelopeOptions options)
    : inner_(std::move(inner))
    , envelope_(std::move(envelope))
    , options_(std::move(options))
{
    if (!inner_)
        throw std::invalid_argument("EnvelopedCheckpointCodec requires an inner codec");
}

Result<Payload> EnvelopedCheckpointCodec::encode(const Checkpoint& checkpoint) const
{
    auto encoded = inner_->encode(checkpoint);
    if (!encoded.isOk())
        return encoded.status();
    return envelope_.encode(*encoded, options_);
}

Result<Checkpoint> EnvelopedCheckpointCodec::decode(const Payload& payload) const
{
    auto decoded = envelope_.decode(payload, options_);
    if (!decoded.isOk())
        return decoded.status();
    return inner_->decode(*decoded);
}

Result<Payload> EnvelopedCheckpointCodec::encodeWrite(const CheckpointWrite& write) const
{
    auto encoded = inner_->encodeWrite(write);
    if (!encoded.isOk())
        return encoded.status();
    return envelope_.encode(*encoded, options_);
}

Result<CheckpointWrite> EnvelopedCheckpointCodec::decodeWrite(const Payload& payload) const
{
    auto decoded = envelope_.decode(payload, options_);
    if (!decoded.isOk())
        return decoded.status();
    return inner_->decodeWrite(*decoded);
}

} // namespace lc
