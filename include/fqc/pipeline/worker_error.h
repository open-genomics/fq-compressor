// =============================================================================
// fq-compressor - Worker Crash Error
// =============================================================================

#pragma once

#include "fqc/common/error.h"

#include <string>
#include <string_view>

namespace fqc::pipeline {

/// Error produced by a pipeline worker's exception barrier. An exception
/// escaping a `std::jthread` body would call `std::terminate` (main's
/// catch-all never sees an exception thrown on another thread), so every
/// worker lambda wraps its body in a barrier that converts the exception into
/// this error and cancels the pipeline cooperatively via `request_stop()`.
/// `stage` names the failing stage ("compress reader", "parallel encoder",
/// ...); `detail` carries the exception's `what()`.
[[nodiscard]] inline auto workerCrashError(std::string_view stage, const char* detail) -> Error {
    return Error{ErrorCode::kInternalError, std::string(stage) + " worker terminated: " + detail};
}

}  // namespace fqc::pipeline
