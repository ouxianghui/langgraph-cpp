#pragma once

#include "foundation/process/process.hpp"

#include "foundation/process/process_common.hh"

namespace lc::process_runner_detail {

[[nodiscard]] Result<ProcessResult> runPlatformProcess(
    const ProcessOptions& options,
    const Clock& clock);

} // namespace lc::process_runner_detail
