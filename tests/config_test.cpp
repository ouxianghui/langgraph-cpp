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
    lgc::ConfigValue text("true");
    auto parsedBool = text.asBool();
    assert(parsedBool.isOk());
    assert(*parsedBool);

    lgc::ConfigValue number("42");
    auto parsedInt = number.asInt64();
    assert(parsedInt.isOk());
    assert(*parsedInt == 42);

    lgc::Config config;
    assert(config.set("llm.model", "gpt-local").isOk());
    assert(config.set("llm.temperature", 0.7).isOk());
    assert(config.set("checkpoint.enabled", true).isOk());
    assert(config.stringValue("llm.model").value() == "gpt-local");
    assert(config.doubleValue("llm.temperature").value() == 0.7);
    assert(config.boolValue("checkpoint.enabled").value());
    assert(config.set(".bad", "x").status().code() == lgc::StatusCode::InvalidArgument);
    assert(config.set("bad key", "x").status().code() == lgc::StatusCode::InvalidArgument);
    assert(config.set("bad/key", "x").status().code() == lgc::StatusCode::InvalidArgument);
    assert(config.set(std::string(257, 'a'), "x").status().code() == lgc::StatusCode::InvalidArgument);

    const auto jsonConfig = lgc::ConfigLoader::fromJsonString(R"({
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
    assert(apiKeyMetadata->source_.type_ == lgc::ConfigSourceType::JsonString);
    assert(apiKeyMetadata->source_.name_ == "<json>");
    assert(apiKeyMetadata->sensitive_);
    assert(jsonConfig->redactedStringValue("llm.api_key").value() == "[REDACTED]");
    assert(jsonConfig->redactedStringValue("paths.checkpoint_dir").value() == "/tmp/lg");

    auto prefixed = lgc::ConfigLoader::fromJsonString(R"({"model":"local"})", "agent");
    assert(prefixed.isOk());
    assert(prefixed->stringValue("agent.model").value() == "local");

    const auto path = std::filesystem::temp_directory_path() / "langgraph_cpp_config_test.json";
    {
        std::ofstream file(path);
        file << R"({"storage":{"path":"/tmp/state.db"}})";
    }
    auto fileConfig = lgc::ConfigLoader::fromJsonFile(path);
    assert(fileConfig.isOk());
    assert(fileConfig->stringValue("storage.path").value() == "/tmp/state.db");
    const auto fileMetadata = fileConfig->metadata("storage.path");
    assert(fileMetadata.has_value());
    assert(fileMetadata->source_.type_ == lgc::ConfigSourceType::JsonFile);
    assert(fileMetadata->source_.name_ == path.string());
    std::filesystem::remove(path);

    lgc::ConfigLoaderOptions strictOptions;
    strictOptions.allowedKeys_ = { "known" };
    assert(lgc::ConfigLoader::fromJsonString(R"({"unknown": 1})", {}, strictOptions).status().code()
        == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigLoader::fromJsonString(R"({"broken": )").status().code()
        == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigLoader::fromJsonString(R"({"value": null})").status().code()
        == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigLoader::fromJsonString(R"({"value": []})").status().code()
        == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigLoader::fromJsonString(R"({"value": [1, "two"]})").status().code()
        == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigLoader::fromJsonString(R"({"value": 1, "value": 2})").status().code()
        == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigLoader::fromJsonString(R"({"bad key": 1})").status().code()
        == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigLoader::fromJsonString(R"({"big": 9223372036854775808})").status().code()
        == lgc::StatusCode::OutOfRange);
    assert(lgc::ConfigValue("nan").asDouble().status().code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::ConfigValue("inf").asDouble().status().code() == lgc::StatusCode::InvalidArgument);

    lgc::ConfigLoaderOptions relaxedNumericArrays;
    relaxedNumericArrays.rejectMixedArray_ = false;
    auto numericArray = lgc::ConfigLoader::fromJsonString(R"({"value": [1, 2.5]})", {}, relaxedNumericArrays);
    assert(numericArray.isOk());
    assert(numericArray->doubleListValue("value").value().at(1) == 2.5);

    lgc::ConfigLoaderOptions limited;
    limited.maxJsonBytes_ = 4;
    assert(lgc::ConfigLoader::fromJsonString(R"({"a":1})", {}, limited).status().code()
        == lgc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxDepth_ = 1;
    assert(lgc::ConfigLoader::fromJsonString(R"({"a":{"b":{"c":1}}})", {}, limited).status().code()
        == lgc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxKeys_ = 1;
    assert(lgc::ConfigLoader::fromJsonString(R"({"a":1,"b":2})", {}, limited).status().code()
        == lgc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxStringBytes_ = 3;
    assert(lgc::ConfigLoader::fromJsonString(R"({"a":"abcd"})", {}, limited).status().code()
        == lgc::StatusCode::ResourceExhausted);
    limited = {};
    limited.maxFileBytes_ = 4;
    {
        std::ofstream file(path);
        file << R"({"a":1})";
    }
    assert(lgc::ConfigLoader::fromJsonFile(path, {}, limited).status().code()
        == lgc::StatusCode::ResourceExhausted);
    std::filesystem::remove(path);
    assert(lgc::ConfigLoader::fromJsonFile(path).status().code() == lgc::StatusCode::NotFound);

    setEnv("LC_TEST_API_KEY", "env-key");
    setEnv("LC_TEST_MAX_TOKENS", "2048");
    setEnv("LC_TEST_STREAM", "on");

    const std::vector<lgc::ConfigEnvBinding> bindings {
        {
            .key_ = "llm.api_key",
            .envName_ = "LC_TEST_API_KEY",
            .type_ = lgc::ConfigValueType::String,
            .required_ = true,
            .sensitive_ = true,
        },
        {
            .key_ = "llm.max_tokens",
            .envName_ = "LC_TEST_MAX_TOKENS",
            .type_ = lgc::ConfigValueType::Int64,
        },
        {
            .key_ = "llm.stream",
            .envName_ = "LC_TEST_STREAM",
            .type_ = lgc::ConfigValueType::Bool,
        },
        {
            .key_ = "paths.cache",
            .envName_ = "LC_TEST_CACHE",
            .type_ = lgc::ConfigValueType::String,
            .defaultValue_ = lgc::ConfigValue("/tmp/cache"),
        },
    };

    auto envConfig = lgc::ConfigLoader::fromEnvironment(bindings);
    assert(envConfig.isOk());
    assert(envConfig->stringValue("llm.api_key").value() == "env-key");
    assert(envConfig->int64Value("llm.max_tokens").value() == 2048);
    assert(envConfig->boolValue("llm.stream").value());
    assert(envConfig->stringValue("paths.cache").value() == "/tmp/cache");
    const auto envMetadata = envConfig->metadata("llm.api_key");
    assert(envMetadata.has_value());
    assert(envMetadata->source_.type_ == lgc::ConfigSourceType::Environment);
    assert(envMetadata->source_.name_ == "LC_TEST_API_KEY");
    assert(envMetadata->sensitive_);
    const auto defaultMetadata = envConfig->metadata("paths.cache");
    assert(defaultMetadata.has_value());
    assert(defaultMetadata->source_.type_ == lgc::ConfigSourceType::Default);

    lgc::Config merged = *jsonConfig;
    assert(lgc::ConfigLoader::mergeEnvironment(merged, bindings).isOk());
    assert(merged.stringValue("llm.api_key").value() == "env-key");
    merged = *jsonConfig;
    assert(lgc::ConfigLoader::mergeEnvironment(merged, bindings, false).isOk());
    assert(merged.stringValue("llm.api_key").value() == "test-key");

    setEnv("LC_TEST_BAD_BOOL", "maybe");
    const std::vector<lgc::ConfigEnvBinding> badBoolBindings {
        {
            .key_ = "bad.bool",
            .envName_ = "LC_TEST_BAD_BOOL",
            .type_ = lgc::ConfigValueType::Bool,
        },
    };
    assert(lgc::ConfigLoader::fromEnvironment(badBoolBindings).status().code()
        == lgc::StatusCode::InvalidArgument);
    unsetEnv("LC_TEST_BAD_BOOL");

    setEnv("LC_TEST_BAD_INT", "not-int");
    const std::vector<lgc::ConfigEnvBinding> badIntBindings {
        {
            .key_ = "bad.int",
            .envName_ = "LC_TEST_BAD_INT",
            .type_ = lgc::ConfigValueType::Int64,
        },
    };
    assert(lgc::ConfigLoader::fromEnvironment(badIntBindings).status().code()
        == lgc::StatusCode::InvalidArgument);
    unsetEnv("LC_TEST_BAD_INT");

    unsetEnv("LC_TEST_API_KEY");
    unsetEnv("LC_TEST_MAX_TOKENS");
    unsetEnv("LC_TEST_STREAM");

    const std::vector<lgc::ConfigEnvBinding> required {
        {
            .key_ = "missing",
            .envName_ = "LC_TEST_REQUIRED_MISSING",
            .type_ = lgc::ConfigValueType::String,
            .required_ = true,
        },
    };
    auto missing = lgc::ConfigLoader::fromEnvironment(required);
    assert(!missing.isOk());
    assert(missing.status().code() == lgc::StatusCode::NotFound);

    return 0;
}
