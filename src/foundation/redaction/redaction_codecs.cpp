#include "foundation/redaction/redactor.hpp"

#include <stdexcept>
#include <utility>

namespace lc {

RedactionStateCodec::RedactionStateCodec(std::shared_ptr<IStateCodec> inner, Redactor redactor)
    : inner_(std::move(inner))
    , redactor_(std::move(redactor))
{
    if (!inner_)
        throw std::invalid_argument("RedactionStateCodec requires an inner codec");
}

Result<Payload> RedactionStateCodec::encode(const State& state) const
{
    auto redacted = redactor_.redact(state);
    if (!redacted.isOk())
        return redacted.status();
    return inner_->encode(*redacted);
}

Result<State> RedactionStateCodec::decode(const Payload& payload) const
{
    return inner_->decode(payload);
}

RedactionCheckpointCodec::RedactionCheckpointCodec(std::shared_ptr<ICheckpointCodec> inner, Redactor redactor)
    : inner_(std::move(inner))
    , redactor_(std::move(redactor))
{
    if (!inner_)
        throw std::invalid_argument("RedactionCheckpointCodec requires an inner codec");
}

Result<Payload> RedactionCheckpointCodec::encode(const Checkpoint& checkpoint) const
{
    auto redacted = redactor_.redact(checkpoint);
    if (!redacted.isOk())
        return redacted.status();
    return inner_->encode(*redacted);
}

Result<Checkpoint> RedactionCheckpointCodec::decode(const Payload& payload) const
{
    return inner_->decode(payload);
}

Result<Payload> RedactionCheckpointCodec::encodeWrite(const CheckpointWrite& write) const
{
    auto redacted = redactor_.redact(write);
    if (!redacted.isOk())
        return redacted.status();
    return inner_->encodeWrite(*redacted);
}

Result<CheckpointWrite> RedactionCheckpointCodec::decodeWrite(const Payload& payload) const
{
    return inner_->decodeWrite(payload);
}

} // namespace lc
