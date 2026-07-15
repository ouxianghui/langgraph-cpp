#include "foundation/secrets/secret_provider.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <cstdlib>
#include <string>

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
    assert(lgc::validateSecretKey("OPENAI_API_KEY").isOk());
    assert(lgc::validateSecretKey("models/openai.api-key").isOk());
    assert(lgc::validateSecretKey("").code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::validateSecretKey("bad name").code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::validateSecretKey(std::string(257, 'a')).code() == lgc::StatusCode::InvalidArgument);

    lgc::Secret secret("sk-test-1234567890");
    assert(!secret.empty());
    assert(secret.stringValue() == "sk-test-1234567890");
    assert(secret.bytes().size() == secret.stringValue().size());
    assert(secret.masked() != secret.stringValue());

    lgc::MemorySecrets memory;
    assert(memory.set("OPENAI_API_KEY", secret).isOk());
    assert(memory.contains("OPENAI_API_KEY"));

    auto loaded = memory.get("OPENAI_API_KEY");
    assert(loaded.isOk());
    assert(loaded->stringValue() == "sk-test-1234567890");
    assert(loaded->masked() == secret.masked());

    assert(memory.get("MISSING").status().code() == lgc::StatusCode::NotFound);
    assert(memory.set("bad name", "x").code() == lgc::StatusCode::InvalidArgument);
    assert(memory.remove("MISSING").code() == lgc::StatusCode::NotFound);
    assert(memory.remove("OPENAI_API_KEY").isOk());
    assert(!memory.contains("OPENAI_API_KEY"));

    assert(memory.set("TOKEN", "token-value").isOk());
    memory.clear();
    assert(!memory.contains("TOKEN"));

    setEnv("LC_SECRET_TEST_TOKEN", "env-token");
    lgc::EnvSecrets env;
    auto envSecret = env.get("LC_SECRET_TEST_TOKEN");
    assert(envSecret.isOk());
    assert(envSecret->stringValue() == "env-token");

    unsetEnv("LC_SECRET_TEST_TOKEN");
    assert(env.get("LC_SECRET_TEST_TOKEN").status().code() == lgc::StatusCode::NotFound);

    return 0;
}
