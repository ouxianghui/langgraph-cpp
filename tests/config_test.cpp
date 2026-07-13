#include "foundation/config/config_loader.hpp"
#include "foundation/config/config_value.hpp"
#include "foundation/config/env.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void setEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name)
{
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

int main()
{
    lc::ConfigValue text("true");
    auto parsedBool = text.asBool();
    assert(parsedBool.isOk());
    assert(*parsedBool);

    lc::ConfigValue number("42");
    auto parsedInt = number.asInt64();
    assert(parsedInt.isOk());
    assert(*parsedInt == 42);

    lc::Config config;
    assert(config.set("llm.model", "gpt-local").isOk());
    assert(config.set("llm.temperature", 0.7).isOk());
    assert(config.set("checkpoint.enabled", true).isOk());
    assert(config.stringValue("llm.model").value() == "gpt-local");
    assert(config.doubleValue("llm.temperature").value() == 0.7);
    assert(config.boolValue("checkpoint.enabled").value());
    assert(config.set(".bad", "x").status().code() == lc::StatusCode::InvalidArgument);
    assert(config.set("bad key", "x").status().code() == lc::StatusCode::InvalidArgument);
    assert(config.set("bad/key", "x").status().code() == lc::StatusCode::InvalidArgument);
    assert(config.set(std::string(257, 'a'), "x").status().code() == lc::StatusCode::InvalidArgument);

    const auto jsonConfig = lc::ConfigLoader::fromJsonString(R"({
        "llm": {
            "api_key": "test-key",
            "temperature": 0.2,
            "max_tokens": 1024,
            "stream": true,
            "stop": ["\n\n", "END"],
            "flags": [true, false],
            "ids": [1, 2, 3],
            "weights": [0.1, 0.2]
        },
        "paths": {
            "checkpoint_dir": "/tmp/lg"
        }
    })");
    assert(jsonConfig.isOk());
    assert(jsonConfig->stringValue("llm.api_key").value() == "test-key");
    assert(jsonConfig->doubleValue("llm.temperature").value() == 0.2);
    assert(jsonConfig->int64Value("llm.max_tokens").value() == 1024);
    assert(jsonConfig->boolValue("llm.stream").value());
    assert(jsonConfig->stringValue("paths.checkpoint_dir").value() == "/tmp/lg");
    const auto stop = jsonConfig->stringListValue("llm.stop");
    assert(stop.isOk());
    assert(stop->size() == 2);
    assert(stop->at(1) == "END");
    const auto flags = jsonConfig->boolListValue("llm.flags");
    assert(flags.isOk());
    assert(flags->size() == 2);
    assert(flags->at(0));
    const auto ids = jsonConfig->int64ListValue("llm.ids");
    assert(ids.isOk());
    assert(ids->at(2) == 3);
    const auto weights = jsonConfig->doubleListValue("llm.weights");
    assert(weights.isOk());
    assert(weights->at(1) == 0.2);
    const auto apiKeyMetadata = jsonConfig->metadata("llm.api_key");
    assert(apiKeyMetadata.has_value());
    assert(apiKeyMetadata->source_.type_ == lc::ConfigSourceType::JsonString);
    assert(apiKeyMetadata->source_.name_ == "<json>");
    assert(apiKeyMetadata->sensitive_);
    assert(jsonConfig->redactedStringValue("llm.api_key").value() == "[REDACTED]");
    assert(jsonConfig->redactedStringValue("paths.checkpoint_dir").value() == "/tmp/lg");

    auto prefixed = lc::ConfigLoader::fromJsonString(R"({"model":"local"})", "agent");
    assert(prefixed.isOk());
    assert(prefixed->stringValue("agent.model").value() == "local");

    const auto path = std::filesystem::temp_directory_path() / "langgraph_cpp_config_test.json";
    {
        std::ofstream file(path);
        file << R"({"storage":{"path":"/tmp/state.db"}})";
    }
    auto fileConfig = lc::ConfigLoader::fromJsonFile(path);
    assert(fileConfig.isOk());
    assert(fileConfig->stringValue("storage.path").value() == "/tmp/state.db");
    const auto fileMetadata = fileConfig->metadata("storage.path");
    assert(fileMetadata.has_value());
    assert(fileMetadata->source_.type_ == lc::ConfigSourceType::JsonFile);
    assert(fileMetadata->source_.name_ == path.string());
    std::filesystem::remove(path);

    lc::ConfigLoaderOptions strictOptions;
    strictOptions.allowedKeys_ = { "known" };
    assert(lc::ConfigLoader::fromJsonString(R"({"unknown": 1})", {}, strictOptions).status().code()
        == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigLoader::fromJsonString(R"({"broken": )").status().code()
        == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigLoader::fromJsonString(R"({"value": null})").status().code()
        == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigLoader::fromJsonString(R"({"value": []})").status().code()
        == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigLoader::fromJsonString(R"({"value": [1, "two"]})").status().code()
        == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigLoader::fromJsonString(R"({"value": 1, "value": 2})").status().code()
        == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigLoader::fromJsonString(R"({"bad key": 1})").status().code()
        == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigLoader::fromJsonString(R"({"big": 9223372036854775808})").status().code()
        == lc::StatusCode::OutOfRange);
    assert(lc::ConfigValue("nan").asDouble().status().code() == lc::StatusCode::InvalidArgument);
    assert(lc::ConfigValue("inf").asDouble().status().code() == lc::StatusCode::InvalidArgument);

    lc::ConfigLoaderOptions relaxedNumericArrays;
    relaxedNumericArrays.rejectMixedArray_ = false;
    auto numericArray = lc::ConfigLoader::fromJsonString(R"({"value": [1, 2.5]})", {}, relaxedNumericArrays);
    assert(numericArray.isOk());
    assert(numericArray->doubleListValue("value").value().at(1) == 2.5);

    lc::ConfigLoaderOptions limited;
    limited.maxJsonBytes_ = 4;
    assert(lc::ConfigLoader::fromJsonString(R"({"a":1})", {}, limited).status().code()
        == lc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxDepth_ = 1;
    assert(lc::ConfigLoader::fromJsonString(R"({"a":{"b":{"c":1}}})", {}, limited).status().code()
        == lc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxKeys_ = 1;
    assert(lc::ConfigLoader::fromJsonString(R"({"a":1,"b":2})", {}, limited).status().code()
        == lc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxStringBytes_ = 3;
    assert(lc::ConfigLoader::fromJsonString(R"({"a":"abcd"})", {}, limited).status().code()
        == lc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxFileBytes_ = 4;
    {
        std::ofstream file(path);
        file << R"({"a":1})";
    }
    assert(lc::ConfigLoader::fromJsonFile(path, {}, limited).status().code()
        == lc::StatusCode::ResourceExhausted);
    std::filesystem::remove(path);
    assert(lc::ConfigLoader::fromJsonFile(path).status().code() == lc::StatusCode::NotFound);

    setEnv("LC_TEST_API_KEY", "env-key");
    setEnv("LC_TEST_MAX_TOKENS", "2048");
    setEnv("LC_TEST_STREAM", "on");

    const std::vector<lc::ConfigEnvBinding> bindings {
        {
            .key_ = "llm.api_key",
            .envName_ = "LC_TEST_API_KEY",
            .type_ = lc::ConfigValueType::String,
            .required_ = true,
            .sensitive_ = true,
        },
        {
            .key_ = "llm.max_tokens",
            .envName_ = "LC_TEST_MAX_TOKENS",
            .type_ = lc::ConfigValueType::Int64,
        },
        {
            .key_ = "llm.stream",
            .envName_ = "LC_TEST_STREAM",
            .type_ = lc::ConfigValueType::Bool,
        },
        {
            .key_ = "paths.cache",
            .envName_ = "LC_TEST_CACHE",
            .type_ = lc::ConfigValueType::String,
            .defaultValue_ = lc::ConfigValue("/tmp/cache"),
        },
    };

    auto envConfig = lc::ConfigLoader::fromEnvironment(bindings);
    assert(envConfig.isOk());
    assert(envConfig->stringValue("llm.api_key").value() == "env-key");
    assert(envConfig->int64Value("llm.max_tokens").value() == 2048);
    assert(envConfig->boolValue("llm.stream").value());
    assert(envConfig->stringValue("paths.cache").value() == "/tmp/cache");
    const auto envMetadata = envConfig->metadata("llm.api_key");
    assert(envMetadata.has_value());
    assert(envMetadata->source_.type_ == lc::ConfigSourceType::Environment);
    assert(envMetadata->source_.name_ == "LC_TEST_API_KEY");
    assert(envMetadata->sensitive_);
    const auto defaultMetadata = envConfig->metadata("paths.cache");
    assert(defaultMetadata.has_value());
    assert(defaultMetadata->source_.type_ == lc::ConfigSourceType::Default);

    lc::Config merged = *jsonConfig;
    assert(lc::ConfigLoader::mergeEnvironment(merged, bindings).isOk());
    assert(merged.stringValue("llm.api_key").value() == "env-key");
    merged = *jsonConfig;
    assert(lc::ConfigLoader::mergeEnvironment(merged, bindings, false).isOk());
    assert(merged.stringValue("llm.api_key").value() == "test-key");

    setEnv("LC_TEST_BAD_BOOL", "maybe");
    const std::vector<lc::ConfigEnvBinding> badBoolBindings {
        {
            .key_ = "bad.bool",
            .envName_ = "LC_TEST_BAD_BOOL",
            .type_ = lc::ConfigValueType::Bool,
        },
    };
    assert(lc::ConfigLoader::fromEnvironment(badBoolBindings).status().code()
        == lc::StatusCode::InvalidArgument);
    unsetEnv("LC_TEST_BAD_BOOL");

    setEnv("LC_TEST_BAD_INT", "not-int");
    const std::vector<lc::ConfigEnvBinding> badIntBindings {
        {
            .key_ = "bad.int",
            .envName_ = "LC_TEST_BAD_INT",
            .type_ = lc::ConfigValueType::Int64,
        },
    };
    assert(lc::ConfigLoader::fromEnvironment(badIntBindings).status().code()
        == lc::StatusCode::InvalidArgument);
    unsetEnv("LC_TEST_BAD_INT");

    unsetEnv("LC_TEST_API_KEY");
    unsetEnv("LC_TEST_MAX_TOKENS");
    unsetEnv("LC_TEST_STREAM");

    const std::vector<lc::ConfigEnvBinding> required {
        {
            .key_ = "missing",
            .envName_ = "LC_TEST_REQUIRED_MISSING",
            .type_ = lc::ConfigValueType::String,
            .required_ = true,
        },
    };
    auto missing = lc::ConfigLoader::fromEnvironment(required);
    assert(!missing.isOk());
    assert(missing.status().code() == lc::StatusCode::NotFound);

    return 0;
}
