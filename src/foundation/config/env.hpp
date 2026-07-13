#pragma once

#include "foundation/status/result.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace lc {

class Env final {
public:
    [[nodiscard]] static std::optional<std::string> get(std::string_view name);
    [[nodiscard]] static std::string valueOr(std::string_view name, std::string fallback);
    [[nodiscard]] static Result<std::string> require(std::string_view name);
};

[[nodiscard]] Status validateEnvName(std::string_view name);

} // namespace lc
