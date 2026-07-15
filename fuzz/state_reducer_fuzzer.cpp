#include "fuzz_common.hpp"
#include "foundation/serialization/json_limits.hpp"
#include "langgraph/state/reducer.hpp"
#include "langgraph/state/state_update.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lgc::fuzz::inputToString(data, size);
        const auto parts = lgc::fuzz::splitInput(input, 2);
        const lgc::JsonDecodeLimits limits {
            .maxBytes_ = 64 * 1024,
            .maxDepth_ = 32,
            .maxStringBytes_ = 16 * 1024,
            .maxArrayItems_ = 4096,
            .maxObjectFields_ = 4096,
            .maxNodes_ = 16 * 1024,
        };

        auto state = lgc::State::fromJson(parts[0], limits);
        auto update = lgc::StateUpdate::fromJson(parts[1], limits);
        if (!state.isOk() || !update.isOk())
            return 0;

        lgc::ReducerRegistry reducers;
        reducers.set("messages", lgc::ReducerKind::AddMessages);
        reducers.set("items", lgc::ReducerKind::Append);
        reducers.set("facts", lgc::ReducerKind::MergeObject);
        reducers.set("overwrite", lgc::ReducerKind::Overwrite);
        (void)lgc::applyStateUpdate(*state, *update, reducers);
        (void)update->toState();
    } catch (...) {
    }
    return 0;
}
