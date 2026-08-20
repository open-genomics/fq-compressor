// =============================================================================
// fq-compressor - Archive Engine
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/format/archive.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace fqc::commands {

inline constexpr std::size_t kDefaultMemoryLimitBytes = format::kDefaultMemoryLimitBytes;
inline constexpr std::size_t kProfileSampleMaxRecords = 50'000;
inline constexpr std::size_t kProfileSampleMaxBases = 512ULL * 1024ULL * 1024ULL;

struct CompressionRequest {
    std::filesystem::path inputPath;
    std::filesystem::path matePath;
    std::filesystem::path outputPath;
    std::optional<format::DatasetProfile> profile;
    std::size_t memoryLimitBytes = kDefaultMemoryLimitBytes;
    std::size_t targetFrameBytes = format::kDefaultTargetFrameBytes;
    int qualityZstdLevel = 1;
    /// Parser workers for parallel parsing; 0 forces the sequential reader.
    /// Parallel parsing only engages on uncompressed regular files
    /// (gzip/stdin/paired always use the sequential path regardless).
    std::size_t parseWorkers = 4;
    bool forceOverwrite = false;

    [[nodiscard]] auto paired() const noexcept -> bool {
        return !matePath.empty();
    }
};

struct DecompressionRequest {
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::size_t memoryLimitBytes = kDefaultMemoryLimitBytes;
    bool forceOverwrite = false;
};

struct OperationStats {
    format::DatasetProfile profile = format::DatasetProfile::kIllumina;
    bool paired = false;
    std::uint64_t frameCount = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t totalBases = 0;
    std::uint64_t inputBytes = 0;
    std::uint64_t outputBytes = 0;
};

class ArchiveEngine {
public:
    ArchiveEngine() = default;

    [[nodiscard]] static auto compress(const CompressionRequest& request) -> Result<OperationStats>;
    [[nodiscard]] static auto decompress(const DecompressionRequest& request)
        -> Result<OperationStats>;
    [[nodiscard]] static auto verify(const std::filesystem::path& inputPath,
                                     std::size_t memoryLimitBytes = kDefaultMemoryLimitBytes)
        -> Result<OperationStats>;
};

}  // namespace fqc::commands
