// =============================================================================
// fq-compressor - Shared Frame Accumulator
// =============================================================================

#pragma once

#include "fqc/common/types.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace fqc::pipeline {

/// Accumulates parsed records into frames by retained-byte budget. Single
/// source of framing rules for both CompressPipeline and ParallelParsePipeline
/// -- the byte-identical archive gate depends on both paths framing the record
/// stream identically.
///
/// Framing rules (unchanged from the original inline reader logic):
/// - a frame closes once its retained bytes reach the target budget;
/// - paired mode only closes on an even record count (pairs stay atomic);
/// - `finish()` flushes whatever remains (end of input / end of chunk).
class FrameAccumulator {
public:
    explicit FrameAccumulator(std::size_t targetFrameBytes, bool paired = false)
        : targetFrameBytes_(targetFrameBytes), paired_(paired) {}

    /// The deterministic framing cost of one record: object + string
    /// *sizes*. Framing MUST be a pure function of record content -- string
    /// *capacities* are not content: libc++ `getline` leaves different
    /// capacities depending on how lines fall across the streambuf's buffer
    /// boundaries, so any capacity-based rule makes frame boundaries depend
    /// on the stream's buffering phase and breaks cross-path framing
    /// identity (parallel parsing reads the same records through differently
    /// phased buffers -- see docs/postmortems/2026-07-28-getline-capacity-framing.md).
    /// The conservative *memory* precheck is unaffected: it lives in
    /// `estimateCompressionPeak` and still accounts capacities.
    [[nodiscard]] static auto retainedBytes(const ReadRecord& record) noexcept -> std::size_t {
        return sizeof(ReadRecord) + record.id.size() + 1 + record.comment.size() + 1 +
            record.sequence.size() + 1 + record.quality.size() + 1;
    }

    /// Append one record; returns the closed frame when the budget was
    /// reached, `std::nullopt` otherwise.
    [[nodiscard]] auto append(ReadRecord record) -> std::optional<std::vector<ReadRecord>> {
        retainedBytes_ += retainedBytes(record);
        frame_.push_back(std::move(record));
        if (retainedBytes_ >= targetFrameBytes_ && (!paired_ || frame_.size() % 2 == 0)) {
            return takeFrame();
        }
        return std::nullopt;
    }

    /// Flush the remainder. Returns `std::nullopt` when nothing is pending.
    [[nodiscard]] auto finish() -> std::optional<std::vector<ReadRecord>> {
        return takeFrame();
    }

private:
    [[nodiscard]] auto takeFrame() -> std::optional<std::vector<ReadRecord>> {
        if (frame_.empty()) {
            return std::nullopt;
        }
        std::vector<ReadRecord> out;
        out.swap(frame_);  // keeps this accumulator's capacity for reuse
        retainedBytes_ = 0;
        return out;
    }

    std::size_t targetFrameBytes_;
    bool paired_;
    std::vector<ReadRecord> frame_;
    std::size_t retainedBytes_ = 0;
};

}  // namespace fqc::pipeline
