// =============================================================================
// fq-compressor - Parallel-Parse Compression Pipeline
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/compress_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>

namespace fqc::pipeline {

/// Scans forward from the stream's current position for the first structurally
/// valid FASTQ record start and returns its absolute offset (`baseOffset` is
/// the absolute offset of the current position), or `std::nullopt` when the
/// chunk holds no record start (the empty tail belongs to the previous
/// chunk's parse region).
///
/// The scan is bounded. A chunk boundary can fall at most three complete lines
/// before the next record start (the straddling record's remaining sequence /
/// '+' / quality lines, which the previous worker parses and validates), so
/// seven non-blank lines always contain the first record start of well-formed
/// input. A candidate is a line starting with '@' that passes the parser-level
/// structural check: third line starts with '+', sequence length == quality
/// length. Content validation (IUPAC charset, etc.) deliberately stays in
/// encodeFrame: a candidate rejected HERE on content would silently skip a
/// record that the sequential path parses and rejects loudly. A line starting
/// with '@' is NOT sufficient on its own -- quality lines may legitimately
/// start with '@' (Phred+33, Q31), so such residue lines are skipped only
/// while they can still be explained as the straddling record's tail.
///
/// Errors: when no structurally valid record starts within the window (and
/// the residue-shape constraint rules out every later candidate), the
/// alignment zone holds data the sequential parser would reject too -- a
/// record with mismatched quality length, a missing '+' line, or garbage
/// beyond the residue. The scan reports `kFormatError` for those instead of
/// silently skipping bytes; only a short EOF tail (<= 3 non-blank lines that
/// could still be the previous record's residue) is skipped, matching the
/// conservative pre-existing behavior for that indistinguishable shape.
///
/// `maxLineBytes` bounds a single scanned line, mirroring FastqParser's cap.
[[nodiscard]] auto findFirstRecordStart(std::istream& input,
                                        std::uint64_t baseOffset,
                                        std::size_t maxLineBytes = io::kDefaultMaxFastqLineBytes)
    -> Result<std::optional<std::uint64_t>>;

/// Compression pipeline whose frame production is data-parallel, in contrast
/// to CompressPipeline's task-parallel reader:
///
/// ```text
/// parser x K  K workers, one per byte chunk of [sampleEnd, fileSize). Each
///             opens its own ifstream, boundary-aligns to the first complete
///             record (see findFirstRecordStart), parses records that START
///             inside its chunk, and tags frames (chunkId, localId). Worker 0
///             additionally seeds its accumulator with the profile sample so
///             framing is continuous across the sample boundary.
///          --[bounded MPMC queue]-->
/// encoder x N  frame -> encodeFrame + compressFrame (markers pass through)
///          --[bounded MPMC queue]-->
/// writer    ChunkOrderer drains in (chunk, local) lexicographic order ->
///           writeCompressedFrame (on-disk frame id from writer counter)
/// ```
/// `N = min(K, kDefaultEncoderParallelism)`: parser fan-out is independently
/// configurable, while the CPU-heavy encoder pool stays within the ordinary
/// compression pipeline's default size.
///
/// Framing note: chunk ends force a frame flush, so with K > 1 the archive's
/// frame split points differ from the sequential pipeline's (identical
/// record order/content, different frame boundaries -- round-trip equal, not
/// archive-byte equal). With K == 1 the framing matches CompressPipeline
/// exactly, which is the byte-identical gate.
class ParallelParsePipeline {
public:
    static constexpr std::size_t kDefaultQueueDepth = 4;
    static constexpr std::size_t kDefaultParallelism = 4;
    static constexpr std::size_t kDefaultEncoderParallelism =
        CompressPipeline::kDefaultEncoderParallelism;

    /// `sampleEndOffset` is the byte offset where parallel parsing starts
    /// (end of the profile sample consumed on the main thread).
    ParallelParsePipeline(std::filesystem::path inputPath,
                          std::uint64_t fileSize,
                          std::size_t targetFrameBytes,
                          std::uint64_t sampleEndOffset,
                          std::size_t parallelism = kDefaultParallelism);

    /// `initialRecords` is the profile sample; it seeds worker 0's frame
    /// accumulator (copied, same as CompressPipeline).
    [[nodiscard]] auto run(std::span<const ReadRecord> initialRecords,
                           format::ArchiveWriter& writer) -> Result<PipelineStats>;

private:
    std::filesystem::path inputPath_;
    std::uint64_t fileSize_;
    std::size_t targetFrameBytes_;
    std::uint64_t sampleEndOffset_;
    std::size_t parserParallelism_;
    std::size_t encoderParallelism_;
};

}  // namespace fqc::pipeline
