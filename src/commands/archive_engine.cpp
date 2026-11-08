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

// Two bounded queues + the InFlightLimiter worker window + source-local frames.
// Source-local frames are not covered by InFlightLimiter: they have not been
// enqueued yet, so the aggregate budget must count them explicitly.
constexpr std::size_t kPipelineQueueFrames =
    2 * (pipeline::CompressPipeline::kDefaultQueueDepth - 1);
// The budget formula assumes one shared queue depth across all three pipelines.
static_assert(pipeline::CompressPipeline::kDefaultQueueDepth ==
              pipeline::ParallelParsePipeline::kDefaultQueueDepth);
static_assert(pipeline::CompressPipeline::kDefaultQueueDepth ==
              pipeline::DecompressPipeline::kDefaultQueueDepth);
constexpr std::size_t kFramePeakToTargetFactor = 4;

[[nodiscard]] constexpr auto maxBufferedFrames(std::size_t inFlightWorkers,
                                               std::size_t sourceLocalFrames) noexcept
    -> std::size_t {
    return kPipelineQueueFrames + inFlightWorkers + sourceLocalFrames;
}

/// Split the non-reserved budget across every frame that can hold data at
/// once. `estimateCompressionPeak` / `estimateDecompressionPeak` are
/// frame-local; the 16 MiB codec/runtime reserve is counted once here.
[[nodiscard]] auto maxFrameBytesFor(std::size_t memoryLimitBytes,
                                    std::size_t bufferedFrames,
                                    std::size_t heldBytes) noexcept -> std::size_t {
    return std::min(format::kDefaultMaxFrameBytes,
                    (memoryLimitBytes - format::kCodecMemoryReserveBytes - heldBytes) /
                        bufferedFrames);
}

[[nodiscard]] constexpr auto nsToMs(std::uint64_t ns) -> double {
    constexpr double kNsPerMillisecond = 1e6;
    return static_cast<double>(ns) / kNsPerMillisecond;
}

// 0 保留流水线默认值（4），避免在命令层再复制一份默认并行度常量。
[[nodiscard]] constexpr auto resolveDecoderWorkers(std::size_t requested) noexcept -> std::size_t {
    return requested == 0 ? pipeline::DecompressPipeline::kDefaultDecoderParallelism : requested;
}

/// Build the decode pipeline with the same budget derivation for `decompress`
/// and `verify`: the per-frame limit depends on the decoder count because every
/// in-flight decoder may hold one frame.
[[nodiscard]] auto makeDecompressPipeline(std::size_t memoryLimitBytes,
                                          std::size_t requestedWorkers)
    -> pipeline::DecompressPipeline {
    const auto workers = resolveDecoderWorkers(requestedWorkers);
    const auto bufferedFrames = maxBufferedFrames(workers, 1);
    return {maxFrameBytesFor(memoryLimitBytes, bufferedFrames, 0), memoryLimitBytes, workers};
}

/// Shared telemetry log: one shape for compression and decompression.
void logPipelineTelemetry(const pipeline::PipelineTelemetry& telemetry) {
    const auto& t = telemetry.timings;
    FQC_LOG_INFO(
        "pipeline stages (ms): reader work={:.1f} push={:.1f} | worker pop={:.1f} "
        "work={:.1f} secondary={:.1f} push={:.1f} | writer pop={:.1f} work={:.1f} | wall={:.1f}",
        nsToMs(t.readerWorkNs),
        nsToMs(t.readerPushNs),
        nsToMs(t.workerPopNs),
        nsToMs(t.workerPrimaryNs),
        nsToMs(t.workerSecondaryNs),
        nsToMs(t.workerPushNs),
        nsToMs(t.writerPopNs),
        nsToMs(t.writerWorkNs),
        nsToMs(t.wallNs));
    const auto& inputQueue = telemetry.inputQueueStats;
    const auto& outputQueue = telemetry.outputQueueStats;
    FQC_LOG_INFO(
        "pipeline queues: input pushes={} pops={} pushWaits={} popWaits={} highWater={} | "
        "output pushes={} pops={} pushWaits={} popWaits={} highWater={}",
        inputQueue.pushes,
        inputQueue.pops,
        inputQueue.pushWaits,
        inputQueue.popWaits,
        inputQueue.highWater,
        outputQueue.pushes,
        outputQueue.pops,
        outputQueue.pushWaits,
        outputQueue.popWaits,
        outputQueue.highWater);
    FQC_LOG_INFO("pipeline in-flight credits: highWater={}", telemetry.inFlightHighWater);
}

void logCompressionObservability(const pipeline::PipelineStats& stats) {
    FQC_LOG_INFO(
        "pipeline workers: parser={} encoder={}", stats.parserWorkers, stats.encoderWorkers);
    logPipelineTelemetry(stats.telemetry);
}

void logDecompressionObservability(const pipeline::DecompressStats& stats) {
    FQC_LOG_INFO("pipeline workers: decoder={}", stats.decoderWorkers);
    logPipelineTelemetry(stats.telemetry);
}

/// The encoder's frame-local peak is roughly 4x the retained FASTQ bytes
/// (record strings + raw streams + compressed bound). Keep the accumulation
/// target at a quarter of the peak budget so a closed frame fits before it is
/// handed to the encoder.
[[nodiscard]] auto targetFrameBytesFor(std::size_t requestedTargetFrameBytes,
                                       std::size_t maxFrameBytes) noexcept -> std::size_t {
    return std::min(requestedTargetFrameBytes, maxFrameBytes / kFramePeakToTargetFactor);
}

/// Conservative per-record held memory. The vector's geometric growth can
/// leave up to one extra slot per element, so count two ReadRecord objects;
/// string capacities are what the allocator actually holds.
[[nodiscard]] auto recordHeldBytes(const ReadRecord& record) noexcept -> std::size_t {
    return 2 * sizeof(ReadRecord) + record.id.capacity() + record.comment.capacity() +
        record.sequence.capacity() + record.quality.capacity();
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
    if (outputPath != "-") {
        std::error_code error;
        const bool exists = std::filesystem::exists(outputPath, error);
        if (error) {
            return makeVoidError(ErrorCode::kIOError,
                                 "cannot inspect output path: " + error.message());
        }
        if (!forceOverwrite && exists) {
            return makeVoidError(ErrorCode::kIOError,
                                 "output already exists: " + outputPath.string());
        }
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

/// Held-memory budget for the profile sample (record slots + string
/// capacities), capped at the historical 512 MiB sample cap. Keeping the
/// sample inside this budget leaves the rest for frames and codec scratch.
[[nodiscard]] auto sampleBudgetBytes(std::size_t memoryLimitBytes) noexcept -> std::size_t {
    return std::min(kProfileSampleMaxBytes,
                    (memoryLimitBytes - format::kCodecMemoryReserveBytes) / 8);
}

}  // namespace

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
    const auto sampleBudget = sampleBudgetBytes(request.memoryLimitBytes);
    std::size_t sampleBytes = 0;
    while (sample.size() < kProfileSampleMaxRecords) {
        FQC_TRY_ASSIGN(pair, io::readRecordPair(primary, mate ? &*mate : nullptr));
        if (!pair.has_value()) {
            break;
        }
        // 采样在整个流水线期间一直存活，也会被复制进首帧；按实际持有字节
        // 限流，避免超大 ID/comment 绕过只按碱基计的旧上限。必须先加入再
        // 判断是否超预算：readRecordPair 已经消费了输入，消费后丢弃会让
        // 并行路径从采样终点继续时少一对记录。允许最后一对越过预算，随后
        // 的显式检查会保证它没有越过整个可用内存。
        sampleBytes += recordHeldBytes(pair->first);
        sample.push_back(std::move(pair->first));
        if (pair->second) {
            sampleBytes += recordHeldBytes(*pair->second);
            sample.push_back(std::move(*pair->second));
        }
        if (sampleBytes >= sampleBudget) {
            break;
        }
    }

    if (sampleBytes >= request.memoryLimitBytes - format::kCodecMemoryReserveBytes) {
        return makeError<OperationStats>(ErrorCode::kUsageError,
                                         "profile sample exceeds the memory budget");
    }

    FQC_TRY_ASSIGN(resolvedProfile,
                   request.profile.has_value() ? Result<format::DatasetProfile>(*request.profile)
                                               : detectProfile(sample));

    OutputTransaction outputTransaction(request.outputPath, request.outputPath != "-");
    FQC_TRY_ASSIGN(output, openOutput(outputTransaction.path()));

    // Parallel parsing: uncompressed regular file, workers > 0. gzip is a pure
    // inflate stream, stdin is not seekable, paired lock-step does not
    // parallelize. Parsing resumes at the sample's exact byte offset.
    std::uint64_t inputFileSize = 0;
    if (request.parseWorkers > 0 && !request.paired() && request.inputPath != "-") {
        inputFileSize = uncompressedRegularFileSize(request.inputPath);
    }
    // Profile sampling may already consume a small file completely. There is
    // no suffix to split in that case, so avoid starting empty parser chunks.
    const bool parallel = inputFileSize > primary.bytesConsumed();
    const auto encoderWorkers = parallel
        ? std::min(request.parseWorkers,
                   pipeline::ParallelParsePipeline::kDefaultEncoderParallelism)
        : pipeline::CompressPipeline::kDefaultEncoderParallelism;
    const auto sourceLocalFrames = parallel ? request.parseWorkers : std::size_t{1};
    // InFlightLimiter is sized from the source fan-out: the sequential reader
    // uses encoder workers, while parallel parsing reserves one credit per
    // parser (K), not per encoder (min(K, 4)). Budget at least the default
    // encoder pool so --parse-workers 1 frames identically to the sequential
    // reader; K=2/3 are slightly conservative, K>=4 matches the limiter.
    // Each parser can additionally hold one frame while waiting for a credit.
    const auto inFlightWorkers = parallel
        ? std::max(request.parseWorkers,
                   pipeline::ParallelParsePipeline::kDefaultEncoderParallelism)
        : encoderWorkers;
    const auto bufferedFrames = maxBufferedFrames(inFlightWorkers, sourceLocalFrames);
    const auto maxFrameBytes =
        maxFrameBytesFor(request.memoryLimitBytes, bufferedFrames, sampleBytes);
    const auto targetFrameBytes = targetFrameBytesFor(request.targetFrameBytes, maxFrameBytes);

    format::ArchiveWriter writer(*output,
                                 {.profile = resolvedProfile,
                                  .paired = request.paired(),
                                  .maxFrameBytes = maxFrameBytes,
                                  .memoryLimitBytes = request.memoryLimitBytes,
                                  .qualityZstdLevel = request.qualityZstdLevel});

    Result<pipeline::PipelineStats> pipelineResult;
    if (parallel) {
        pipeline::ParallelParsePipeline parallelEngine(request.inputPath,
                                                       inputFileSize,
                                                       targetFrameBytes,
                                                       primary.bytesConsumed(),
                                                       request.parseWorkers);
        pipelineResult = parallelEngine.run(std::span<const ReadRecord>{sample}, writer);
    } else {
        const pipeline::CompressPipeline pipelineEngine(targetFrameBytes, request.paired());
        std::istream* mateStream = mateStreamPtr ? mateStreamPtr.get() : nullptr;
        pipelineResult = pipelineEngine.run(
            *primaryStream, mateStream, std::span<const ReadRecord>{sample}, writer);
    }
    FQC_TRY_ASSIGN(pipelineStats, std::move(pipelineResult));
    logCompressionObservability(pipelineStats);
    output.reset();
    FQC_TRY(outputTransaction.commit(request.forceOverwrite));
    return toOperationStats(
        pipelineStats.metadata, pipelineStats.archive, true, pipelineStats.logicalBytes);
}

auto ArchiveEngine::decompress(const DecompressionRequest& request) -> Result<OperationStats> {
    FQC_TRY(validateMemoryLimit(request.memoryLimitBytes));
    FQC_TRY(validateOutput(request.inputPath, request.outputPath, request.forceOverwrite));
    FQC_TRY_ASSIGN(input, io::openInputFile(request.inputPath));
    OutputTransaction outputTransaction(request.outputPath, request.outputPath != "-");
    FQC_TRY_ASSIGN(output, openOutput(outputTransaction.path()));
    const pipeline::DecompressPipeline pipelineEngine =
        makeDecompressPipeline(request.memoryLimitBytes, request.decoderWorkers);
    FQC_TRY_ASSIGN(
        stats,
        pipelineEngine.run(*input, [&](const std::vector<ReadRecord>& records) -> VoidResult {
            for (const auto& record : records) {
                FQC_TRY(writeFastqRecord(*output, record));
            }
            return {};
        }));
    logDecompressionObservability(stats);
    output->flush();
    if (!*output) {
        return makeError<OperationStats>(ErrorCode::kIOError, "failed to flush decompressed FASTQ");
    }
    output.reset();
    FQC_TRY(outputTransaction.commit(request.forceOverwrite));
    return toOperationStats(stats.metadata, stats.archive, false, stats.logicalBytes);
}

auto ArchiveEngine::verify(const std::filesystem::path& inputPath,
                           std::size_t memoryLimitBytes,
                           std::size_t decoderWorkers) -> Result<OperationStats> {
    FQC_TRY(validateMemoryLimit(memoryLimitBytes));
    FQC_TRY_ASSIGN(input, io::openInputFile(inputPath));
    // Same pipeline as decompress with a counting-only sink: full decode and
    // validation, nothing written.
    const pipeline::DecompressPipeline pipelineEngine =
        makeDecompressPipeline(memoryLimitBytes, decoderWorkers);
    FQC_TRY_ASSIGN(stats,
                   pipelineEngine.run(
                       *input, [](const std::vector<ReadRecord>&) -> VoidResult { return {}; }));
    logDecompressionObservability(stats);
    return toOperationStats(stats.metadata, stats.archive, false, stats.logicalBytes);
}

}  // namespace fqc::commands
