// =============================================================================
// fq-compressor - Archive Engine
// =============================================================================

#include "fqc/commands/archive_engine.h"

#include "fqc/commands/profile.h"
#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/io/compressed_stream.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/log.h"
#include "fqc/pipeline/compress_pipeline.h"
#include "fqc/pipeline/decompress_pipeline.h"
#include "fqc/pipeline/parallel_parse_pipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fqc::commands {

namespace {

constexpr std::size_t kEngineMemoryReserveBytes = std::size_t{16} * 1024 * 1024;
constexpr std::size_t kMinimumMemoryLimitBytes = std::size_t{64} * 1024 * 1024;

class OutputTransaction {
public:
    OutputTransaction(std::filesystem::path finalPath, bool useStaging)
        : finalPath_(std::move(finalPath)), stagingPath_(finalPath_) {
        if (useStaging && finalPath_ != "-") {
            static std::atomic<std::uint64_t> sequence = 0;
            const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            stagingPath_ += ".tmp." + std::to_string(timestamp) + "." +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        }
    }

    ~OutputTransaction() {
        if (!committed_ && stagingPath_ != finalPath_) {
            std::error_code ignored;
            std::filesystem::remove(stagingPath_, ignored);
        }
    }

    OutputTransaction(const OutputTransaction&) = delete;
    auto operator=(const OutputTransaction&) -> OutputTransaction& = delete;
    OutputTransaction(OutputTransaction&&) = delete;
    auto operator=(OutputTransaction&&) -> OutputTransaction& = delete;

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& {
        return stagingPath_;
    }

    [[nodiscard]] auto commit(bool forceOverwrite) -> VoidResult {
        if (stagingPath_ == finalPath_) {
            committed_ = true;
            return {};
        }

        std::error_code error;
        const auto outputExists = std::filesystem::exists(finalPath_, error);
        if (error) {
            return makeVoidError(ErrorCode::kIOError,
                                 "failed to inspect output path: " + error.message());
        }
        if (outputExists) {
            if (!forceOverwrite) {
                return makeVoidError(ErrorCode::kIOError,
                                     "output already exists: " + finalPath_.string());
            }
        }
        std::filesystem::rename(stagingPath_, finalPath_, error);
        if (error && forceOverwrite && outputExists) {
            error.clear();
            std::filesystem::remove(finalPath_, error);
            if (!error) {
                std::filesystem::rename(stagingPath_, finalPath_, error);
            }
        }
        if (error) {
            return makeVoidError(ErrorCode::kIOError,
                                 "failed to publish output: " + error.message());
        }
        committed_ = true;
        return {};
    }

private:
    std::filesystem::path finalPath_;
    std::filesystem::path stagingPath_;
    bool committed_ = false;
};

[[nodiscard]] auto validateMemoryLimit(std::size_t memoryLimitBytes) -> VoidResult {
    if (memoryLimitBytes < kMinimumMemoryLimitBytes) {
        return makeVoidError(ErrorCode::kUsageError, "memory limit must be at least 64 MiB");
    }
    return {};
}

[[nodiscard]] auto maxFrameBytesFor(std::size_t memoryLimitBytes) noexcept -> std::size_t {
    return std::min(format::kDefaultMaxFrameBytes,
                    (memoryLimitBytes - kEngineMemoryReserveBytes) / 4);
}

[[nodiscard]] constexpr auto nsToMs(std::uint64_t ns) -> double {
    constexpr double kNsPerMillisecond = 1e6;
    return static_cast<double>(ns) / kNsPerMillisecond;
}

/// Per-stage wall-clock plus queue snapshots. Info level, so `-q` suppresses it.
void logPipelineObservability(const pipeline::PipelineStats& stats) {
    const auto& t = stats.timings;
    FQC_LOG_INFO(
        "pipeline stages (ms): reader parse={:.1f} push={:.1f} | encoder pop={:.1f} "
        "encode={:.1f} zstd={:.1f} push={:.1f} | writer pop={:.1f} write={:.1f} | wall={:.1f}",
        nsToMs(t.readerParseNs),
        nsToMs(t.readerPushNs),
        nsToMs(t.encoderPopNs),
        nsToMs(t.encoderEncodeNs),
        nsToMs(t.encoderCompressNs),
        nsToMs(t.encoderPushNs),
        nsToMs(t.writerPopNs),
        nsToMs(t.writerWriteNs),
        nsToMs(t.wallNs));
    const auto& q1 = stats.queue1Stats;
    const auto& q2 = stats.queue2Stats;
    FQC_LOG_INFO(
        "pipeline queues: q1 pushes={} pops={} pushWaits={} popWaits={} highWater={} | "
        "q2 pushes={} pops={} pushWaits={} popWaits={} highWater={}",
        q1.pushes,
        q1.pops,
        q1.pushWaits,
        q1.popWaits,
        q1.highWater,
        q2.pushes,
        q2.pops,
        q2.pushWaits,
        q2.popWaits,
        q2.highWater);
}

[[nodiscard]] auto targetFrameBytesFor(const CompressionRequest& request) noexcept -> std::size_t {
    return std::min(request.targetFrameBytes,
                    (request.memoryLimitBytes - kEngineMemoryReserveBytes) / 8);
}

/// File size when `path` is a regular uncompressed file (parallel parsing needs
/// random access to uncompressed bytes). Compressed input or a failed stat/probe
/// yields 0, which routes to the sequential reader.
[[nodiscard]] auto uncompressedRegularFileSize(const std::filesystem::path& path) -> std::uint64_t {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return 0;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return 0;
    }
    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
        return 0;
    }
    std::array<std::uint8_t, 8> magic{};
    probe.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    const auto bytesRead = probe.gcount();
    if (io::detectCompressionFormat({magic.data(), static_cast<std::size_t>(bytesRead)}) !=
        io::CompressionFormat::kNone) {
        return 0;
    }
    return size;
}

[[nodiscard]] auto validateOutput(const std::filesystem::path& inputPath,
                                  const std::filesystem::path& outputPath,
                                  bool forceOverwrite) -> VoidResult {
    if (inputPath.empty() || outputPath.empty()) {
        return makeVoidError(ErrorCode::kUsageError, "input and output paths are required");
    }
    if (inputPath != "-" && outputPath != "-" && inputPath == outputPath) {
        return makeVoidError(ErrorCode::kUsageError, "input and output paths must differ");
    }
    if (!forceOverwrite && outputPath != "-" && std::filesystem::exists(outputPath)) {
        return makeVoidError(ErrorCode::kIOError, "output already exists: " + outputPath.string());
    }
    return {};
}

[[nodiscard]] auto openOutput(const std::filesystem::path& path)
    -> Result<std::unique_ptr<std::ostream>> {
    if (path == "-") {
        return std::unique_ptr<std::ostream>(std::make_unique<std::ostream>(std::cout.rdbuf()));
    }
    auto stream = std::make_unique<std::ofstream>(path, std::ios::binary);
    if (!stream->is_open()) {
        return makeError<std::unique_ptr<std::ostream>>(
            ErrorCode::kIOError, "cannot create output file: " + path.string());
    }
    return std::unique_ptr<std::ostream>(std::move(stream));
}

[[nodiscard]] auto writeFastqRecord(std::ostream& output, const ReadRecord& record) -> VoidResult {
    output << '@' << record.id;
    if (!record.comment.empty()) {
        // comment 已含前导分隔空格（见 FastqParser），原样拼接即可，勿再补空格。
        output << record.comment;
    }
    output << '\n' << record.sequence << "\n+\n" << record.quality << '\n';
    if (!output) {
        return makeVoidError(ErrorCode::kIOError, "failed to write decompressed FASTQ");
    }
    return {};
}

[[nodiscard]] auto toOperationStats(const format::ArchiveMetadata& metadata,
                                    const format::ArchiveStats& archiveStats,
                                    bool compressing,
                                    std::uint64_t logicalBytes) -> OperationStats {
    return {
        .profile = metadata.profile,
        .paired = metadata.paired,
        .frameCount = archiveStats.frameCount,
        .recordCount = archiveStats.recordCount,
        .totalBases = archiveStats.totalBases,
        .inputBytes = compressing ? logicalBytes : archiveStats.encodedBytes,
        .outputBytes = compressing ? archiveStats.encodedBytes : logicalBytes,
    };
}

[[nodiscard]] auto sampleLimitBytes(std::size_t memoryLimitBytes) noexcept -> std::size_t {
    return std::min(kProfileSampleMaxBases, (memoryLimitBytes - kEngineMemoryReserveBytes) / 8);
}

}  // namespace

[[nodiscard]] auto toArchiveStats(const pipeline::DecompressStats& stats) -> format::ArchiveStats {
    return {
        .frameCount = stats.frameCount,
        .recordCount = stats.recordCount,
        .totalBases = stats.totalBases,
        .encodedBytes = stats.encodedBytes,
    };
}

auto ArchiveEngine::compress(const CompressionRequest& request) -> Result<OperationStats> {
    FQC_TRY(validateMemoryLimit(request.memoryLimitBytes));
    if (request.targetFrameBytes == 0) {
        return makeError<OperationStats>(ErrorCode::kUsageError,
                                         "target frame size must be positive");
    }
    FQC_TRY(validateOutput(request.inputPath, request.outputPath, request.forceOverwrite));
    if (request.paired() && request.inputPath == "-" && request.matePath == "-") {
        return makeError<OperationStats>(ErrorCode::kUsageError,
                                         "paired inputs cannot both use stdin");
    }

    FQC_TRY_ASSIGN(primaryStream, io::openInputFile(request.inputPath));
    io::FastqParser primary(*primaryStream);
    std::unique_ptr<std::istream> mateStreamPtr;
    std::optional<io::FastqParser> mate;
    if (request.paired()) {
        FQC_TRY_ASSIGN(mateStream, io::openInputFile(request.matePath));
        mateStreamPtr = std::move(mateStream);
        mate.emplace(*mateStreamPtr);
    }

    std::vector<ReadRecord> sample;
    const auto maxSampleBytes = sampleLimitBytes(request.memoryLimitBytes);
    std::size_t sampledBases = 0;
    while (sample.size() < kProfileSampleMaxRecords && sampledBases < maxSampleBytes) {
        FQC_TRY_ASSIGN(pair, io::readRecordPair(primary, mate ? &*mate : nullptr));
        if (!pair.has_value()) {
            break;
        }
        sampledBases += pair->first.sequence.size();
        sample.push_back(std::move(pair->first));
        if (pair->second) {
            sampledBases += pair->second->sequence.size();
            sample.push_back(std::move(*pair->second));
        }
    }

    FQC_TRY_ASSIGN(resolvedProfile,
                   request.profile.has_value() ? Result<format::DatasetProfile>(*request.profile)
                                               : detectProfile(sample));

    OutputTransaction outputTransaction(request.outputPath, request.outputPath != "-");
    FQC_TRY_ASSIGN(output, openOutput(outputTransaction.path()));
    format::ArchiveWriter writer(*output,
                                 {.profile = resolvedProfile,
                                  .paired = request.paired(),
                                  .maxFrameBytes = maxFrameBytesFor(request.memoryLimitBytes),
                                  .memoryLimitBytes = request.memoryLimitBytes,
                                  .qualityZstdLevel = request.qualityZstdLevel});

    // Parallel parsing: uncompressed regular file, workers > 0. gzip is a pure
    // inflate stream, stdin is not seekable, paired lock-step does not
    // parallelize. Parsing resumes at the sample's exact byte offset.
    std::uint64_t inputFileSize = 0;
    if (request.parseWorkers > 0 && !request.paired() && request.inputPath != "-") {
        inputFileSize = uncompressedRegularFileSize(request.inputPath);
    }
    Result<pipeline::PipelineStats> pipelineResult;
    if (inputFileSize > 0) {
        pipeline::ParallelParsePipeline parallelEngine(request.inputPath,
                                                       inputFileSize,
                                                       targetFrameBytesFor(request),
                                                       primary.bytesConsumed(),
                                                       request.parseWorkers);
        pipelineResult = parallelEngine.run(std::span<const ReadRecord>{sample}, writer);
    } else {
        const pipeline::CompressPipeline pipelineEngine(targetFrameBytesFor(request),
                                                        request.paired());
        std::istream* mateStream = mateStreamPtr ? mateStreamPtr.get() : nullptr;
        pipelineResult = pipelineEngine.run(
            *primaryStream, mateStream, std::span<const ReadRecord>{sample}, writer);
    }
    FQC_TRY_ASSIGN(pipelineStats, std::move(pipelineResult));
    logPipelineObservability(pipelineStats);
    FQC_TRY(writer.finish());
    output.reset();
    FQC_TRY(outputTransaction.commit(request.forceOverwrite));
    return toOperationStats(writer.metadata(), writer.stats(), true, pipelineStats.logicalBytes);
}

auto ArchiveEngine::decompress(const DecompressionRequest& request) -> Result<OperationStats> {
    FQC_TRY(validateMemoryLimit(request.memoryLimitBytes));
    FQC_TRY(validateOutput(request.inputPath, request.outputPath, request.forceOverwrite));
    FQC_TRY_ASSIGN(input, io::openInputFile(request.inputPath));
    OutputTransaction outputTransaction(request.outputPath, request.outputPath != "-");
    FQC_TRY_ASSIGN(output, openOutput(outputTransaction.path()));
    std::uint64_t logicalBytes = 0;
    const pipeline::DecompressPipeline pipelineEngine(maxFrameBytesFor(request.memoryLimitBytes),
                                                      request.memoryLimitBytes);
    FQC_TRY_ASSIGN(
        stats,
        pipelineEngine.run(*input, [&](const std::vector<ReadRecord>& records) -> VoidResult {
            for (const auto& record : records) {
                logicalBytes += canonicalFastqBytes(record);
                FQC_TRY(writeFastqRecord(*output, record));
            }
            return {};
        }));
    output->flush();
    if (!*output) {
        return makeError<OperationStats>(ErrorCode::kIOError, "failed to flush decompressed FASTQ");
    }
    output.reset();
    FQC_TRY(outputTransaction.commit(request.forceOverwrite));
    return toOperationStats(stats.metadata, toArchiveStats(stats), false, logicalBytes);
}

auto ArchiveEngine::verify(const std::filesystem::path& inputPath,
                           std::size_t memoryLimitBytes) -> Result<OperationStats> {
    FQC_TRY(validateMemoryLimit(memoryLimitBytes));
    FQC_TRY_ASSIGN(input, io::openInputFile(inputPath));
    // Same pipeline as decompress with a counting-only sink: full decode and
    // validation, nothing written.
    std::uint64_t logicalBytes = 0;
    const pipeline::DecompressPipeline pipelineEngine(maxFrameBytesFor(memoryLimitBytes),
                                                      memoryLimitBytes);
    FQC_TRY_ASSIGN(
        stats,
        pipelineEngine.run(*input, [&](const std::vector<ReadRecord>& records) -> VoidResult {
            for (const auto& record : records) {
                logicalBytes += canonicalFastqBytes(record);
            }
            return {};
        }));
    return toOperationStats(stats.metadata, toArchiveStats(stats), false, logicalBytes);
}

}  // namespace fqc::commands
