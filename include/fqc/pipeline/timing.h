// =============================================================================
// fq-compressor - Pipeline Timing Helper
// =============================================================================

#pragma once

#include <chrono>
#include <cstdint>

namespace fqc::pipeline {

using Clock = std::chrono::steady_clock;

[[nodiscard]] inline auto nanosSince(Clock::time_point start) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

}  // namespace fqc::pipeline
