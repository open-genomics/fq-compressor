// =============================================================================
// fq-compressor - FQC v2 Sequential Archive
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace fqc::format {

inline constexpr std::uint16_t kArchiveVersion = 2;
inline constexpr std::size_t kDefaultTargetFrameBytes = std::size_t{64} * 1024 * 1024;
inline constexpr std::size_t kDefaultMaxFrameBytes = std::size_t{512} * 1024 * 1024;
inline constexpr std::size_t kDefaultMemoryLimitBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;

enum class DatasetProfile : std::uint8_t {
    kIllumina = 1,
    kOnt = 2,
    kPacBioHiFi = 3,
    kPacBioClr = 4,
};

[[nodiscard]] constexpr auto profileToString(DatasetProfile profile) noexcept -> std::string_view {
    switch (profile) {
        case DatasetProfile::kIllumina:
            return "illumina";
        case DatasetProfile::kOnt:
            return "ont";
        case DatasetProfile::kPacBioHiFi:
            return "pacbio-hifi";
        case DatasetProfile::kPacBioClr:
            return "pacbio-clr";
    }
    return "unknown";
}

[[nodiscard]] auto parseProfile(std::string_view value) -> Result<DatasetProfile>;

struct ArchiveOptions {
    DatasetProfile profile = DatasetProfile::kIllumina;
    bool paired = false;
    std::size_t maxFrameBytes = kDefaultMaxFrameBytes;
    std::size_t memoryLimitBytes = kDefaultMemoryLimitBytes;
    /// zstd level for the quality stream only; ID/sequence streams stay at
    /// level 1 (stage I: quality is the ratio bottleneck, the other streams
    /// gain little from higher levels). zstd frames are self-describing, so
    /// the decoder needs no change and the on-disk codec IDs are untouched.
    int qualityZstdLevel = 1;
};

struct ArchiveMetadata {
    std::uint16_t version = kArchiveVersion;
    DatasetProfile profile = DatasetProfile::kIllumina;
    bool paired = false;
};

struct ArchiveStats {
    std::uint64_t frameCount = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t encodedBytes = 0;
};

/// A frame's three raw (pre-compression) logical streams plus the metadata the
/// next stage needs. Produced by `encodeFrame` (CPU-only, no I/O) and consumed
/// by `compressFrame` (zstd, also CPU-only). Splitting encode from compress
/// keeps each pipeline stage's work item small and independently schedulable.
struct EncodedFrame {
    std::vector<std::uint8_t> rawIds;
    std::vector<std::uint8_t> rawSequences;
    std::vector<std::uint8_t> rawQualities;
    std::uint32_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t checksum = 0;
};

/// A frame's three zstd-compressed streams plus everything the writer needs to
/// emit the on-disk frame: raw sizes for the frame header, and the logical
/// checksum computed over the *raw* streams (carried through from
/// `encodeFrame`, so the end-to-end integrity story is unchanged). Produced by
/// `compressFrame` (pure computation, safe on a worker thread) and consumed by
/// `ArchiveWriter::writeCompressedFrame` (frame header + write, no CPU-heavy
/// work left).
struct CompressedFrame {
    std::vector<std::uint8_t> ids;
    std::vector<std::uint8_t> sequences;
    std::vector<std::uint8_t> qualities;
    std::uint64_t rawIdsSize = 0;
    std::uint64_t rawSequencesSize = 0;
    std::uint64_t rawQualitiesSize = 0;
    std::uint32_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t checksum = 0;
};

/// Encode records into an `EncodedFrame` (validate + measure + 2-bit pack +
/// checksum). Pure computation, no I/O -- safe to call on a worker thread.
[[nodiscard]] auto encodeFrame(std::span<const ReadRecord> records, const ArchiveOptions& options)
    -> Result<std::unique_ptr<EncodedFrame>>;

/// Compress an `EncodedFrame`'s three raw streams with zstd (one
/// `ZSTD_compress` call per stream): ID/sequence at level 1, quality at
/// `qualityZstdLevel`. Takes ownership of the input and releases each raw
/// stream right after compressing it, so the resident peak during the call
/// stays at raw x3 + one `ZSTD_compressBound` scratch instead of
/// raw x3 + bound x3. Pure computation -- safe to call on a worker thread.
///
/// The output is deterministic for a fixed zstd build, level and input: same
/// bytes in, same bytes out. The pipeline relies on this for its
/// byte-identical archive gate (see roadmap stage F).
[[nodiscard]] auto compressFrame(std::unique_ptr<EncodedFrame> frame,
                                 int qualityZstdLevel) -> Result<std::unique_ptr<CompressedFrame>>;

/// A frame's three still-compressed payloads plus the header fields decoding
/// needs. Produced by `ArchiveReader::readRawFrame` (I/O + all bounds and
/// memory prechecks, no CPU-heavy work) and consumed by `decodeRawFrame`
/// (pure computation, safe on a worker thread). Sizes are pre-validated
/// against the reader's configured limits, so they fit `std::size_t`.
struct RawFrame {
    std::vector<std::uint8_t> ids;        // zstd-compressed
    std::vector<std::uint8_t> sequences;  // zstd-compressed
    std::vector<std::uint8_t> qualities;  // zstd-compressed
    std::size_t rawIdsSize = 0;
    std::size_t rawSequencesSize = 0;
    std::size_t rawQualitiesSize = 0;
    std::uint32_t recordCount = 0;
    std::uint64_t checksum = 0;  // expected logical checksum over the raw streams
};

/// A fully decoded and verified frame. `checksum` is the per-frame logical
/// checksum (verified against the header during decoding); the *rolling*
/// global checksum is order-dependent and must be accumulated by the caller
/// in frame order (see roadmap stage G).
struct DecodedFrame {
    std::vector<ReadRecord> records;
    std::uint32_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t checksum = 0;
};

/// Footer fields as parsed from the stream. Structural checks (size, trailing
/// bytes) happen on the reading side; *validation* (totals and global
/// checksum against accumulated state) is the caller's job, because the
/// accumulated state lives wherever decoding happened (single thread or the
/// ordered end of a pipeline).
struct ArchiveFooter {
    std::uint64_t frameCount = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t globalChecksum = 0;
};

/// Decompress and verify a `RawFrame`: zstd-decompress each stream (checking
/// the declared raw sizes), verify the logical checksum, and decode the raw
/// streams into validated records. Pure computation, no shared state -- safe
/// to call on a worker thread.
[[nodiscard]] auto decodeRawFrame(const RawFrame& frame) -> Result<DecodedFrame>;

/// One step of the archive's rolling global checksum. Order-dependent: apply
/// strictly in ascending frame-id order (the sequential reader does this
/// internally; a pipeline must do it on its ordered end).
[[nodiscard]] auto advanceGlobalChecksum(std::uint64_t current,
                                         std::uint64_t frameChecksum) -> std::uint64_t;

class ArchiveWriter {
public:
    ArchiveWriter(std::ostream& output, ArchiveOptions options);

    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    [[nodiscard]] auto writeFrame(std::span<const ReadRecord> records) -> VoidResult;
    /// Write a `CompressedFrame` produced by `compressFrame`: assemble the
    /// frame header (raw/compressed sizes, logical checksum), write header +
    /// three payloads, then update stats and the rolling checksum. No CPU-heavy
    /// work remains here -- the writer is I/O-bound by design. Owns `output_`,
    /// so call from a single thread.
    [[nodiscard]] auto writeCompressedFrame(std::unique_ptr<CompressedFrame> frame) -> VoidResult;
    [[nodiscard]] auto finish() -> VoidResult;

    [[nodiscard]] auto metadata() const noexcept -> const ArchiveMetadata& {
        return metadata_;
    }

    [[nodiscard]] auto stats() const noexcept -> const ArchiveStats& {
        return stats_;
    }

    [[nodiscard]] auto options() const noexcept -> const ArchiveOptions& {
        return options_;
    }

private:
    [[nodiscard]] auto ensureHeader() -> VoidResult;

    std::ostream& output_;
    ArchiveOptions options_;
    ArchiveMetadata metadata_;
    ArchiveStats stats_;
    std::uint64_t globalChecksum_ = 0;
    bool headerWritten_ = false;
    bool finished_ = false;
};

class ArchiveReader {
public:
    explicit ArchiveReader(std::istream& input,
                           std::size_t maxFrameBytes = kDefaultMaxFrameBytes,
                           std::size_t memoryLimitBytes = kDefaultMemoryLimitBytes);

    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;

    [[nodiscard]] auto open() -> Result<ArchiveMetadata>;

    /// Sequential all-in-one frame read: composed of `readRawFrame` +
    /// `decodeRawFrame` + in-order stats/rolling-checksum accumulation +
    /// footer validation. Returns nullopt once the footer has been reached
    /// and validated.
    [[nodiscard]] auto readFrame() -> Result<std::optional<std::vector<ReadRecord>>>;

    /// I/O-only half of `readFrame`: reads one frame header + its compressed
    /// payloads with every bounds/memory precheck applied (no zstd, no
    /// decode). Returns nullopt once the footer is reached; the parsed
    /// (structurally checked, not yet validated) footer is then available
    /// via `footer()`. Designed for pipeline readers: the CPU-heavy half
    /// runs on decoder workers, footer validation on the ordered end.
    [[nodiscard]] auto readRawFrame() -> Result<std::optional<RawFrame>>;

    /// The parsed footer. Error if the footer has not been reached yet.
    /// Validation against accumulated stats/checksum is the caller's job.
    [[nodiscard]] auto footer() const -> Result<ArchiveFooter>;

    [[nodiscard]] auto metadata() const noexcept -> const ArchiveMetadata& {
        return metadata_;
    }

    [[nodiscard]] auto stats() const noexcept -> const ArchiveStats& {
        return stats_;
    }

    [[nodiscard]] auto finished() const noexcept -> bool {
        return finished_;
    }

private:
    std::istream& input_;
    std::size_t maxFrameBytes_;
    std::size_t memoryLimitBytes_;
    ArchiveMetadata metadata_;
    ArchiveStats stats_;
    std::optional<ArchiveFooter> footer_;
    std::uint64_t globalChecksum_ = 0;
    bool opened_ = false;
    bool finished_ = false;
};

}  // namespace fqc::format
