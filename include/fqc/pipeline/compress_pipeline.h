// =============================================================================
// fq-compressor - Concurrent Compression Pipeline
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/format/archive.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>

namespace fqc::pipeline {

struct PipelineStats {
    std::size_t frameCount = 0;
    std::size_t recordCount = 0;
    std::uint64_t logicalBytes = 0;
};

/// Concurrent pipeline: a reader thread parses FASTQ and accumulates bounded
/// frames, N encoder workers encode frames in parallel, and a writer thread
/// compresses and writes them in frame-id order (via a reorder buffer). Two
/// bounded MPMC queues decouple the stages: parsing overlaps CPU-bound
/// encoding, and parallel encoding overlaps compression/I/O. Frame boundaries
/// are naturally independent (see ARCHITECTURE.md), so encoders need no shared
/// mutable state -- each encodes its own frame, tagged with a monotonic id the
/// reorder buffer uses to restore in-order submission to the writer.
class CompressPipeline {
public:
    static constexpr std::size_t kDefaultQueueDepth = 4;
    static constexpr std::size_t kDefaultEncoderParallelism = 4;

    explicit CompressPipeline(std::size_t targetFrameBytes,
                              bool paired = false,
                              std::size_t parallelism = kDefaultEncoderParallelism);

    /// Run the pipeline. `initialRecords` are emitted first (e.g. a profile
    /// sample already consumed by the caller), then records are streamed from
    /// `primary` (and `mate` when paired). On any stage failure the shared
    /// stop_source cancels the others through their stop_token, so no stage
    /// deadlocks waiting on a full push or empty pop.
    [[nodiscard]] auto run(std::istream& primary,
                           std::istream* mate,
                           std::span<const ReadRecord> initialRecords,
                           format::ArchiveWriter& writer) -> Result<PipelineStats>;

private:
    std::size_t targetFrameBytes_;
    bool paired_;
    std::size_t parallelism_;
};

}  // namespace fqc::pipeline
