// =============================================================================
// fq-compressor - Shared Pipeline Telemetry
// =============================================================================

#pragma once

#include "fqc/format/archive.h"
#include "fqc/pipeline/mpmc_queue.h"

#include <cstddef>
#include <cstdint>

namespace fqc::pipeline {

/// Wall-clock breakdown shared by all frame pipelines.
///
/// `workerPrimaryNs` is the primary CPU stage (encode for compression, decode
/// for decompression). `workerSecondaryNs` is the optional second CPU stage; the
/// compression pipelines put zstd there, decompression leaves it at zero.
/// Keeping the names stage-neutral lets both directions expose the same
/// observability shape without pretending a decoder is an encoder.
struct StageTimings {
    std::uint64_t readerWorkNs = 0;
    std::uint64_t readerPushNs = 0;
    std::uint64_t workerPopNs = 0;
    std::uint64_t workerPrimaryNs = 0;
    std::uint64_t workerSecondaryNs = 0;
    std::uint64_t workerPushNs = 0;
    std::uint64_t writerPopNs = 0;
    std::uint64_t writerWorkNs = 0;
    std::uint64_t wallNs = 0;
};

/// Runtime telemetry produced by a pipeline run. Counts are advisory and are
/// never used for synchronization; read them after the run has joined.
struct PipelineTelemetry {
    StageTimings timings;
    QueueStats inputQueueStats;   // reader -> worker
    QueueStats outputQueueStats;  // worker -> writer
    /// Peak number of limiter credits held by submitted frames. Source-local
    /// frames waiting for a credit are not included.
    std::size_t inFlightHighWater = 0;
};

/// Result payload common to compression and decompression.
///
/// `archive` is the canonical format-level counter set (same type used by
/// `ArchiveWriter` / `ArchiveReader`), so callers no longer translate between
/// three near-identical count structs. `logicalBytes` is the canonical FASTQ
/// byte count observed by the pipeline; it is independent of frame boundaries
/// and compressed sizes.
struct PipelineRunStats {
    format::ArchiveMetadata metadata;
    format::ArchiveStats archive;
    PipelineTelemetry telemetry;
    std::uint64_t logicalBytes = 0;
};

}  // namespace fqc::pipeline
