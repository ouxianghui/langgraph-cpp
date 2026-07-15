#pragma once

#include "foundation/process/process.hpp"

#include "foundation/process/process_common.hh"

namespace lgc::process_runner_detail {

[[nodiscard]] Result<ProcessResult> runPlatformProcess(
    const ProcessOptions& options,
    const Clock& clock);

} // namespace lgc::process_runner_detail
