#include "fuzz_common.hpp"
#include "foundation/serialization/json_limits.hpp"
#include "langgraph/state/reducer.hpp"
#include "langgraph/state/state_update.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    try {
        const auto input = lc::fuzz::inputToString(data, size);
        const auto parts = lc::fuzz::splitInput(input, 2);
        const lc::JsonDecodeLimits limits {
            .maxBytes_ = 64 * 1024,
            .maxDepth_ = 32,
            .maxStringBytes_ = 16 * 1024,
            .maxArrayItems_ = 4096,
            .maxObjectFields_ = 4096,
            .maxNodes_ = 16 * 1024,
        };

        auto state = lc::State::fromJson(parts[0], limits);
        auto update = lc::StateUpdate::fromJson(parts[1], limits);
        if (!state.isOk() || !update.isOk())
            return 0;

        lc::ReducerRegistry reducers;
        reducers.set("messages", lc::ReducerKind::AddMessages);
        reducers.set("items", lc::ReducerKind::Append);
        reducers.set("facts", lc::ReducerKind::MergeObject);
        reducers.set("overwrite", lc::ReducerKind::Overwrite);
        (void)lc::applyStateUpdate(*state, *update, reducers);
        (void)update->toState();
    } catch (...) {
    }
    return 0;
}
