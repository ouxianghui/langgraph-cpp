#include "foundation/observability/tracing.hpp"

#include "foundation/observability/tracing_common.hh"
#include "foundation/redaction/redactor.hpp"

#include <stdexcept>
#include <utility>

namespace lc {
namespace {
using tracing_detail::makeChildContext;
using tracing_detail::makeRootContext;
using tracing_detail::normalizeOptions;
using tracing_detail::redactAttributeObject;
using tracing_detail::validateAttributes;
using tracing_detail::validateText;
}

Span::State::State(SpanRecord data, std::shared_ptr<ITraceSink> sink, TraceOptions options)
    : data_(std::move(data))
    , sink_(std::move(sink))
    , options_(normalizeOptions(std::move(options)))
{
}

Span::Span(std::shared_ptr<State> state)
    : state_(std::move(state))
{
}

Span::~Span()
{
    if (state_)
        (void)end();
}

Span::Span(Span&& other) noexcept
    : state_(std::move(other.state_))
{
}

Span& Span::operator=(Span&& other) noexcept
{
    if (this == &other)
        return *this;

    if (state_)
        (void)end();
    state_ = std::move(other.state_);
    return *this;
}

bool Span::isValid() const noexcept
{
    return static_cast<bool>(state_);
}

TraceContext Span::context() const
{
    if (!state_)
        return {};
    std::lock_guard lock(state_->mutex_);
    return state_->data_.context_;
}

Status Span::lastEndStatus() const
{
    if (!state_)
        return Status::ok();
    std::lock_guard lock(state_->mutex_);
    return state_->lastEndStatus_;
}

Status Span::setAttribute(std::string key, nlohmann::json value)
{
    if (!state_)
        return Status::failedPrecondition("span is not valid");

    std::lock_guard lock(state_->mutex_);
    if (state_->ended_)
        return Status::failedPrecondition("span is already ended");

    value = redactAttributeObject(key, std::move(value), state_->options_);
    nlohmann::json attributes = state_->data_.attributes_;
    attributes[std::move(key)] = std::move(value);
    if (auto status = validateAttributes(attributes, state_->options_.limits_); !status.isOk())
        return status;

    state_->data_.attributes_ = std::move(attributes);
    return Status::ok();
}

Status Span::addEvent(std::string name, nlohmann::json attributes)
{
    if (!state_)
        return Status::failedPrecondition("span is not valid");

    std::lock_guard lock(state_->mutex_);
    if (state_->ended_)
        return Status::failedPrecondition("span is already ended");
    if (state_->options_.limits_.maxEvents_ != 0 && state_->data_.events_.size() >= state_->options_.limits_.maxEvents_)
        return Status::resourceExhausted("span has too many events");

    if (state_->options_.redact_ && state_->options_.redactor_)
        attributes = state_->options_.redactor_->redact(attributes);

    if (auto status = validateText(name, "span event name", state_->options_.limits_.maxNameLength_); !status.isOk())
        return status;
    if (auto status = validateAttributes(attributes, state_->options_.limits_); !status.isOk())
        return status;

    state_->data_.events_.push_back(SpanEvent {
        .name_ = std::move(name),
        .attributes_ = std::move(attributes),
        .timestamp_ = state_->options_.clock_->now(),
    });
    return Status::ok();
}

Status Span::setStatus(SpanStatus status, std::string message)
{
    if (!state_)
        return Status::failedPrecondition("span is not valid");

    std::lock_guard lock(state_->mutex_);
    if (state_->ended_)
        return Status::failedPrecondition("span is already ended");
    if (state_->options_.redact_ && state_->options_.redactor_)
        message = state_->options_.redactor_->redact(message);
    if (auto textStatus = validateText(
            message,
            "span status message",
            state_->options_.limits_.maxStatusMessageLength_,
            true);
        !textStatus.isOk()) {
        return textStatus;
    }
    state_->data_.status_ = status;
    state_->data_.statusMessage_ = std::move(message);
    return Status::ok();
}

Status Span::end()
{
    if (!state_)
        return Status::ok();

    std::shared_ptr<ITraceSink> sink;
    SpanRecord data;
    {
        std::lock_guard lock(state_->mutex_);
        if (state_->ended_)
            return Status::ok();
        state_->ended_ = true;
        state_->data_.endedAt_ = state_->options_.clock_->now();
        data = state_->data_;
        sink = state_->sink_;
    }

    if (!sink) {
        auto status = Status::failedPrecondition("span trace sink is not valid");
        std::lock_guard lock(state_->mutex_);
        state_->lastEndStatus_ = status;
        return status;
    }
    auto status = sink->recordSpan(std::move(data));
    {
        std::lock_guard lock(state_->mutex_);
        state_->lastEndStatus_ = status;
    }
    return status;
}

Tracer::Tracer(std::shared_ptr<ITraceSink> sink, TraceOptions options)
    : sink_(std::move(sink))
    , options_(normalizeOptions(std::move(options)))
{
    if (!sink_)
        throw std::invalid_argument("Tracer requires a trace sink");
}

Result<Span> Tracer::startSpan(
    std::string name,
    std::optional<TraceContext> parent,
    nlohmann::json attributes) const
{
    if (auto status = validateText(name, "span name", options_.limits_.maxNameLength_); !status.isOk())
        return status;

    if (options_.redact_ && options_.redactor_)
        attributes = options_.redactor_->redact(attributes);
    if (auto status = validateAttributes(attributes, options_.limits_); !status.isOk())
        return status;

    Result<TraceContext> context = parent.has_value()
        ? makeChildContext(*parent, options_)
        : makeRootContext(options_);
    if (!context.isOk())
        return context.status();

    SpanRecord data {
        .context_ = std::move(*context),
        .name_ = std::move(name),
        .attributes_ = std::move(attributes),
        .startedAt_ = options_.clock_->now(),
    };

    return Span(std::make_shared<Span::State>(std::move(data), sink_, options_));
}

} // namespace lc
