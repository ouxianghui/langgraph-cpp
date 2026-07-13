#include "foundation/redaction/redactor.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <utility>

namespace lc {
namespace {

[[nodiscard]] std::string normalizeKey(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte))
            out.push_back(static_cast<char>(std::tolower(byte)));
    }
    return out;
}

[[nodiscard]] bool containsAny(std::string_view haystack, const std::vector<std::string>& needles)
{
    return std::any_of(needles.begin(), needles.end(), [&](const std::string& needle) {
        return !needle.empty() && haystack.find(needle) != std::string_view::npos;
    });
}

[[nodiscard]] std::string regexReplace(
    std::string input,
    const std::regex& pattern,
    std::string_view replacement,
    RedactionReport& stats)
{
    std::string output;
    std::sregex_iterator it(input.begin(), input.end(), pattern);
    const std::sregex_iterator end;
    if (it == end)
        return input;

    std::size_t cursor = 0;
    for (; it != end; ++it) {
        const auto& match = *it;
        output.append(input, cursor, static_cast<std::size_t>(match.position()) - cursor);

        if (match.size() >= 2 && match[1].matched) {
            output.append(match[1].str());
            output.append(replacement);
        } else {
            output.append(replacement);
        }

        cursor = static_cast<std::size_t>(match.position() + match.length());
        ++stats.strings_;
    }
    output.append(input, cursor, std::string::npos);
    return output;
}

[[nodiscard]] std::string redactValuePatterns(
    std::string value,
    const RedactionConfig& options,
    RedactionReport& stats)
{
    if (value.empty() || !options.redactStringValues_)
        return value;

    if (options.maxStringLengthToScan_ != 0 && value.size() > options.maxStringLengthToScan_) {
        ++stats.strings_;
        return options.replacement_;
    }

    static const std::regex bearerPattern(
        R"(\b((?:Bearer|Basic)\s+)[A-Za-z0-9._~+/=-]{8,})",
        std::regex::icase | std::regex::optimize);
    static const std::regex querySecretPattern(
        R"(\b((?:api[_-]?key|access[_-]?token|refresh[_-]?token|id[_-]?token|token|secret|password|pwd)=)[^&\s]+)",
        std::regex::icase | std::regex::optimize);
    static const std::regex skPattern(
        R"(\b(?:sk|pk|rk|sess|ghp|gho|ghu|github_pat)-[A-Za-z0-9_\-]{8,}\b)",
        std::regex::icase | std::regex::optimize);
    static const std::regex jwtPattern(
        R"(\b[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b)",
        std::regex::optimize);
    static const std::regex awsAccessKeyPattern(
        R"(\b(?:AKIA|ASIA)[0-9A-Z]{16}\b)",
        std::regex::optimize);
    static const std::regex emailPattern(
        R"(\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b)",
        std::regex::icase | std::regex::optimize);
    static const std::regex creditCardPattern(
        R"(\b(?:\d[ -]*?){13,19}\b)",
        std::regex::optimize);

    value = regexReplace(std::move(value), bearerPattern, options.replacement_, stats);
    value = regexReplace(std::move(value), querySecretPattern, options.replacement_, stats);
    value = regexReplace(std::move(value), skPattern, options.replacement_, stats);
    value = regexReplace(std::move(value), jwtPattern, options.replacement_, stats);
    value = regexReplace(std::move(value), awsAccessKeyPattern, options.replacement_, stats);

    if (options.redactEmailAddresses_)
        value = regexReplace(std::move(value), emailPattern, options.replacement_, stats);

    if (options.redactCreditCardNumbers_)
        value = regexReplace(std::move(value), creditCardPattern, options.replacement_, stats);

    return value;
}

[[nodiscard]] nlohmann::json redactedScalar(const RedactionConfig& options)
{
    return options.replacement_;
}

} // namespace

RedactionConfig RedactionConfig::defaults()
{
    return RedactionConfig {
        .replacement_ = "[REDACTED]",
        .sensitiveKeys_ = {
            "api_key",
            "apikey",
            "access_token",
            "refresh_token",
            "id_token",
            "token",
            "auth_token",
            "authorization",
            "cookie",
            "set_cookie",
            "x_api_key",
            "password",
            "passwd",
            "pwd",
            "secret",
            "client_secret",
            "private_key",
            "session",
            "session_id",
            "jwt",
            "credential",
            "credentials",
        },
        .sensitiveKeySubstrings_ = {
            "apikey",
            "accesstoken",
            "refreshtoken",
            "idtoken",
            "authtoken",
            "authorization",
            "password",
            "clientsecret",
            "privatekey",
            "secret",
            "credential",
        },
    };
}

Redactor::Redactor(RedactionConfig options)
    : config_(std::move(options))
{
    if (config_.replacement_.empty())
        config_.replacement_ = "[REDACTED]";

    normalizedSensitiveKeys_.reserve(config_.sensitiveKeys_.size());
    for (const auto& key : config_.sensitiveKeys_) {
        auto normalized = normalizeKey(key);
        if (!normalized.empty())
            normalizedSensitiveKeys_.push_back(std::move(normalized));
    }

    normalizedSensitiveKeySubstrings_.reserve(config_.sensitiveKeySubstrings_.size());
    for (const auto& key : config_.sensitiveKeySubstrings_) {
        auto normalized = normalizeKey(key);
        if (!normalized.empty())
            normalizedSensitiveKeySubstrings_.push_back(std::move(normalized));
    }
}

const RedactionConfig& Redactor::config() const noexcept
{
    return config_;
}

bool Redactor::sensitiveKey(std::string_view key) const
{
    if (!config_.redactSensitiveKeys_)
        return false;

    const auto normalized = normalizeKey(key);
    if (normalized.empty())
        return false;

    if (std::find(
            normalizedSensitiveKeys_.begin(),
            normalizedSensitiveKeys_.end(),
            normalized)
        != normalizedSensitiveKeys_.end()) {
        return true;
    }

    return containsAny(normalized, normalizedSensitiveKeySubstrings_);
}

RedactionResult<std::string> Redactor::redactWithReport(std::string_view value) const
{
    RedactionReport stats;
    auto redacted = redactValuePatterns(std::string(value), config_, stats);
    return RedactionResult<std::string> {
        .value_ = std::move(redacted),
        .report_ = stats,
    };
}

RedactionResult<std::string> Redactor::redactWithReport(const std::string& value) const
{
    return redactWithReport(std::string_view(value));
}

std::string Redactor::redact(std::string_view value) const
{
    return redactWithReport(value).value_;
}

std::string Redactor::redact(const std::string& value) const
{
    return redact(std::string_view(value));
}

std::string Redactor::redact(const char* value) const
{
    return redact(std::string_view(value == nullptr ? "" : value));
}

RedactionResult<nlohmann::json> Redactor::redactWithReport(const nlohmann::json& value) const
{
    return redactJsonValue(value, 0);
}

nlohmann::json Redactor::redact(const nlohmann::json& value) const
{
    return redactWithReport(value).value_;
}

RuntimeEvent Redactor::redact(RuntimeEvent event) const
{
    event.message_ = redact(event.message_);
    event.payload_ = redact(event.payload_);
    return event;
}

SpanEvent Redactor::redact(SpanEvent event) const
{
    event.attributes_ = redact(event.attributes_);
    return event;
}

SpanRecord Redactor::redact(SpanRecord span) const
{
    span.attributes_ = redact(span.attributes_);
    span.statusMessage_ = redact(span.statusMessage_);
    for (auto& event : span.events_)
        event = redact(std::move(event));
    return span;
}

Result<State> Redactor::redact(const State& state) const
{
    try {
        auto parsed = nlohmann::json::parse(state.json());
        auto redacted = redact(parsed);
        return State::fromJson(redacted.dump());
    } catch (const nlohmann::json::exception& error) {
        return Status::invalidArgument(std::string("state json parse failed: ") + error.what());
    }
}

Result<CheckpointTask> Redactor::redact(const CheckpointTask& task) const
{
    auto redacted = task;
    if (redacted.state_.has_value()) {
        auto state = redact(*redacted.state_);
        if (!state.isOk())
            return state.status();
        redacted.state_ = std::move(*state);
    }
    if (redacted.error_.has_value())
        redacted.error_ = redact(*redacted.error_);
    redacted.interrupts_ = redact(redacted.interrupts_);
    redacted.metadata_ = redact(redacted.metadata_);
    return redacted;
}

Result<CheckpointWrite> Redactor::redact(const CheckpointWrite& write) const
{
    auto redacted = write;
    auto update = redact(write.update_);
    if (!update.isOk())
        return update.status();
    redacted.update_ = std::move(*update);
    redacted.metadata_ = redact(redacted.metadata_);
    for (auto& task : redacted.nextTasks_) {
        auto nextTask = redact(task);
        if (!nextTask.isOk())
            return nextTask.status();
        task = std::move(*nextTask);
    }
    return redacted;
}

Result<Checkpoint> Redactor::redact(const Checkpoint& checkpoint) const
{
    auto redacted = checkpoint;

    auto state = redact(checkpoint.state_);
    if (!state.isOk())
        return state.status();
    redacted.state_ = std::move(*state);
    redacted.metadata_ = redact(redacted.metadata_);

    for (auto& task : redacted.nextTasks_) {
        auto nextTask = redact(task);
        if (!nextTask.isOk())
            return nextTask.status();
        task = std::move(*nextTask);
    }

    for (auto& write : redacted.writes_) {
        auto redactedWrite = redact(write);
        if (!redactedWrite.isOk())
            return redactedWrite.status();
        write = std::move(*redactedWrite);
    }

    for (auto& write : redacted.pendingWrites_) {
        auto redactedWrite = redact(write);
        if (!redactedWrite.isOk())
            return redactedWrite.status();
        write = std::move(*redactedWrite);
    }

    return redacted;
}

RedactionResult<nlohmann::json> Redactor::redactJsonValue(
    const nlohmann::json& value,
    std::size_t depth) const
{
    std::size_t nodes = 0;
    std::size_t outputBytes = 0;
    return redactJsonValue(value, depth, nodes, outputBytes);
}

RedactionResult<nlohmann::json> Redactor::redactJsonValue(
    const nlohmann::json& value,
    std::size_t depth,
    std::size_t& nodes,
    std::size_t& outputBytes) const
{
    if (depth > config_.maxDepth_) {
        return RedactionResult<nlohmann::json> {
            .value_ = redactedScalar(config_),
            .report_ = RedactionReport { .fields_ = 1 },
        };
    }

    if (config_.maxJsonNodes_ != 0 && ++nodes > config_.maxJsonNodes_) {
        return RedactionResult<nlohmann::json> {
            .value_ = redactedScalar(config_),
            .report_ = RedactionReport { .fields_ = 1 },
        };
    }

    auto accountOutput = [&](std::size_t bytes) {
        if (config_.maxOutputBytes_ == 0)
            return true;
        if (outputBytes > config_.maxOutputBytes_ || bytes > config_.maxOutputBytes_ - outputBytes)
            return false;
        outputBytes += bytes;
        return true;
    };

    if (value.is_object()) {
        if (config_.maxObjectSize_ != 0 && value.size() > config_.maxObjectSize_) {
            return RedactionResult<nlohmann::json> {
                .value_ = redactedScalar(config_),
                .report_ = RedactionReport { .fields_ = 1 },
            };
        }

        nlohmann::json out = nlohmann::json::object();
        RedactionReport stats;

        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!accountOutput(it.key().size())) {
                return RedactionResult<nlohmann::json> {
                    .value_ = redactedScalar(config_),
                    .report_ = RedactionReport { .fields_ = stats.fields_ + 1, .strings_ = stats.strings_ },
                };
            }
            if (sensitiveKey(it.key())) {
                out[it.key()] = redactedScalar(config_);
                ++stats.fields_;
                if (!accountOutput(config_.replacement_.size())) {
                    return RedactionResult<nlohmann::json> {
                        .value_ = redactedScalar(config_),
                        .report_ = RedactionReport { .fields_ = stats.fields_ + 1, .strings_ = stats.strings_ },
                    };
                }
                continue;
            }

            auto redacted = redactJsonValue(*it, depth + 1, nodes, outputBytes);
            out[it.key()] = std::move(redacted.value_);
            stats.fields_ += redacted.report_.fields_;
            stats.strings_ += redacted.report_.strings_;
        }

        return RedactionResult<nlohmann::json> {
            .value_ = std::move(out),
            .report_ = stats,
        };
    }

    if (value.is_array()) {
        if (config_.maxArraySize_ != 0 && value.size() > config_.maxArraySize_) {
            return RedactionResult<nlohmann::json> {
                .value_ = redactedScalar(config_),
                .report_ = RedactionReport { .fields_ = 1 },
            };
        }

        nlohmann::json out = nlohmann::json::array();
        RedactionReport stats;

        for (const auto& item : value) {
            auto redacted = redactJsonValue(item, depth + 1, nodes, outputBytes);
            out.push_back(std::move(redacted.value_));
            stats.fields_ += redacted.report_.fields_;
            stats.strings_ += redacted.report_.strings_;
        }

        return RedactionResult<nlohmann::json> {
            .value_ = std::move(out),
            .report_ = stats,
        };
    }

    if (value.is_string()) {
        auto redacted = redactWithReport(value.get_ref<const std::string&>());
        if (!accountOutput(redacted.value_.size())) {
            return RedactionResult<nlohmann::json> {
                .value_ = redactedScalar(config_),
                .report_ = RedactionReport {
                    .fields_ = 1,
                    .strings_ = redacted.report_.strings_,
                },
            };
        }
        return RedactionResult<nlohmann::json> {
            .value_ = std::move(redacted.value_),
            .report_ = redacted.report_,
        };
    }

    if (!accountOutput(32)) {
        return RedactionResult<nlohmann::json> {
            .value_ = redactedScalar(config_),
            .report_ = RedactionReport { .fields_ = 1 },
        };
    }

    return RedactionResult<nlohmann::json> {
        .value_ = value,
        .report_ = {},
    };
}
} // namespace lc
