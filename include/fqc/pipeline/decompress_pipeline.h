// =============================================================================
// fq-compressor - Concurrent Decompression Pipeline
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/format/archive.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <vector>

namespace fqc::pipeline {

struct DecompressStats {
    format::ArchiveMetadata metadata;
    std::uint64_t frameCount = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t encodedBytes = 0;
};

/// Concurrent decompression pipeline (roadmap stage G), the mirror image of
/// `CompressPipeline`:
///
/// ```text
/// reader   ArchiveReader::open + readRawFrame loop (I/O + prechecks, each
///          raw frame tagged with a monotonic id) --[bounded MPMC queue]-->
/// decoder  N workers: decodeRawFrame (zstd + checksum + record decode,
///          CPU-heavy, completes out of order) --[bounded MPMC queue]-->
/// writer   reorder buffer submits in frame-id order -> rolling global
///          checksum accumulation -> per-frame RecordSink invocation
/// ```
///
/// Correctness invariants (stage G trap list):
/// - The per-frame logical checksum is order-independent and is verified on
///   the decoder workers; the *rolling* global checksum is a chained,
///   order-dependent accumulation and is computed ONLY on the ordered writer
///   thread. Footer totals/global-checksum validation happens after the join,
///   against the writer's accumulated values.
/// - The decode-side memory precheck runs inside `readRawFrame` before a
///   frame ever enters the queue, so the in-flight envelope matches the
///   compression pipeline (2 x queue depth + N workers).
///
/// The `RecordSink` is the pluggable "generic stage" surface: `decompress`
/// sinks records to a FASTQ writer, `verify` sinks them to a byte counter --
/// same pipeline, first reuse. Invoked once per frame, strictly in frame-id
/// order, on the writer thread. It returns a `VoidResult` so an output
/// failure (e.g. disk full) cancels the pipeline through the stop_source.
class DecompressPipeline {
public:
    static constexpr std::size_t kDefaultQueueDepth = 4;
    static constexpr std::size_t kDefaultDecoderParallelism = 4;

    using RecordSink = std::function<VoidResult(std::vector<ReadRecord>)>;

    DecompressPipeline(std::size_t maxFrameBytes,
                       std::size_t memoryLimitBytes,
                       std::size_t parallelism = kDefaultDecoderParallelism);

    /// Run the pipeline to EOF. On any stage failure the shared stop_source
    /// cancels the others through their stop_token (same pattern as
    /// CompressPipeline). Footer validation failures surface as errors only
    /// after all frames were decoded and emitted in order.
    [[nodiscard]] auto run(std::istream& input, RecordSink sink) -> Result<DecompressStats>;

private:
    std::size_t maxFrameBytes_;
    std::size_t memoryLimitBytes_;
    std::size_t parallelism_;
};

}  // namespace fqc::pipeline
