#include "fuzz_common.hpp"
#include "langgraph/graph/run_config.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        const auto parts = lc::fuzz::splitInput(input, 2);
        const auto configJson = lc::fuzz::parseJsonOrDiscard(parts[0]);
        const auto patchJson = lc::fuzz::parseJsonOrDiscard(parts[1]);

        if (!configJson.is_discarded()) {
            auto config = lc::RunnableConfig::fromJson(configJson);
            if (config.isOk()) {
                (void)config->toJson();
                if (!patchJson.is_discarded())
                    (void)lc::patchRunnableConfig(*config, patchJson);
                (void)lc::mergeRunnableConfigs(std::vector<lc::RunnableConfig> { *config });
                (void)lc::applyRunnableConfig(lc::RunOptions {}, *config);
            }
        }
        if (!patchJson.is_discarded())
            (void)lc::RunnableConfig::fromJson(patchJson);
    } catch (...) {
    }
    return 0;
}
