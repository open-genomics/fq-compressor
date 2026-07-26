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
/// writer needs to emit it. Produced by `encodeFrame` (CPU-only, no I/O) and
/// consumed by `ArchiveWriter::writeEncodedFrame` (zstd + write). Splitting the
/// two lets the pipeline overlap encoding with compression/I/O.
struct EncodedFrame {
    std::vector<std::uint8_t> rawIds;
    std::vector<std::uint8_t> rawSequences;
    std::vector<std::uint8_t> rawQualities;
    std::uint32_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t checksum = 0;
};

/// Encode records into an `EncodedFrame` (validate + measure + 2-bit pack +
/// checksum). Pure computation, no I/O -- safe to call on a worker thread.
[[nodiscard]] auto encodeFrame(std::span<const ReadRecord> records, const ArchiveOptions& options)
    -> Result<std::unique_ptr<EncodedFrame>>;

class ArchiveWriter {
public:
    ArchiveWriter(std::ostream& output, ArchiveOptions options);

    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    [[nodiscard]] auto writeFrame(std::span<const ReadRecord> records) -> VoidResult;
    /// Compress (zstd x3) and write an `EncodedFrame` produced by `encodeFrame`,
    /// then update stats/checksum. Owns `output_`, so call from a single thread.
    [[nodiscard]] auto writeEncodedFrame(std::unique_ptr<EncodedFrame> frame) -> VoidResult;
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
    [[nodiscard]] auto readFrame() -> Result<std::optional<std::vector<ReadRecord>>>;

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
    [[nodiscard]] auto readCompressed(std::uint64_t compressedSize,
                                      std::uint64_t rawSize) -> Result<std::vector<std::uint8_t>>;
    [[nodiscard]] auto readFooter(std::span<const std::uint8_t> magicBytes)
        -> Result<std::optional<std::vector<ReadRecord>>>;

    std::istream& input_;
    std::size_t maxFrameBytes_;
    std::size_t memoryLimitBytes_;
    ArchiveMetadata metadata_;
    ArchiveStats stats_;
    std::uint64_t globalChecksum_ = 0;
    bool opened_ = false;
    bool finished_ = false;
};

}  // namespace fqc::format
