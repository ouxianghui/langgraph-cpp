#include "fuzz_common.hpp"
#include "langgraph/graph/run_config.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lgc::fuzz::inputToString(data, size);
        const auto parts = lgc::fuzz::splitInput(input, 2);
        const auto configJson = lgc::fuzz::parseJsonOrDiscard(parts[0]);
        const auto patchJson = lgc::fuzz::parseJsonOrDiscard(parts[1]);

        if (!configJson.is_discarded()) {
            auto config = lgc::RunnableConfig::fromJson(configJson);
            if (config.isOk()) {
                (void)config->toJson();
                if (!patchJson.is_discarded())
                    (void)lgc::patchRunnableConfig(*config, patchJson);
                (void)lgc::mergeRunnableConfigs(std::vector<lgc::RunnableConfig> { *config });
                (void)lgc::applyRunnableConfig(lgc::RunOptions {}, *config);
            }
        }
        if (!patchJson.is_discarded())
            (void)lgc::RunnableConfig::fromJson(patchJson);
    } catch (...) {
    }
    return 0;
}
