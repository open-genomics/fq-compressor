// =============================================================================
// fq-compressor - Concurrent Compression Pipeline
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/format/archive.h"
#include "fqc/pipeline/mpmc_queue.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>

namespace fqc::pipeline {

/// Wall-clock breakdown per pipeline stage (roadmap stage E). Each thread
/// accumulates `steady_clock` samples in locals and merges with one relaxed
/// fetch_add at exit, so the measurement stays off the synchronization hot
/// path. Encoder fields are the sum across all N workers. Since stage F the
/// zstd work lives inside the encoder workers (`encoderCompressNs`), so the
/// writer section should approach pure I/O (`writerWriteNs`).
struct StageTimings {
    std::uint64_t readerParseNs = 0;      // reader: parse + accumulate records
    std::uint64_t readerPushNs = 0;       // reader: queue1 push (incl. backpressure)
    std::uint64_t encoderPopNs = 0;       // encoders: queue1 pop wait
    std::uint64_t encoderEncodeNs = 0;    // encoders: encodeFrame (2-bit pack+checksum)
    std::uint64_t encoderCompressNs = 0;  // encoders: compressFrame (zstd x3, stage F)
    std::uint64_t encoderPushNs = 0;      // encoders: queue2 push
    std::uint64_t writerPopNs = 0;        // writer: queue2 pop wait
    std::uint64_t writerWriteNs = 0;      // writer: frame header + write + checksum
    std::uint64_t wallNs = 0;             // run() total wall clock
};

struct PipelineStats {
    std::size_t frameCount = 0;
    std::size_t recordCount = 0;
    std::uint64_t logicalBytes = 0;
    StageTimings timings;
    QueueStats queue1Stats;
    QueueStats queue2Stats;
};

/// Concurrent pipeline: a reader thread parses FASTQ and accumulates bounded
/// frames, N encoder workers encode *and* zstd-compress frames in parallel
/// (stage F: compression moved into the worker pool), and a writer thread
/// emits them in frame-id order (via a reorder buffer). Two bounded MPMC
/// queues decouple the stages: parsing overlaps CPU-bound encode/compress,
/// and parallel CPU work overlaps the writer's pure I/O. Frame boundaries are
/// naturally independent (see ARCHITECTURE.md), so encoders need no shared
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
