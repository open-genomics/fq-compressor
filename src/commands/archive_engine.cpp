// =============================================================================
// fq-compressor - V2 Sequential Archive Engine
// =============================================================================

#include "fqc/commands/archive_engine.h"

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/io/compressed_stream.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/compress_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
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

[[nodiscard]] auto lowerCopy(std::string_view value) -> std::string {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] auto containsAny(std::string_view value,
                               std::initializer_list<std::string_view> needles) -> bool {
    return std::ranges::any_of(needles,
                               [value](std::string_view needle) { return value.contains(needle); });
}

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

[[nodiscard]] auto targetFrameBytesFor(const CompressionRequest& request) noexcept -> std::size_t {
    return std::min(request.targetFrameBytes,
                    (request.memoryLimitBytes - kEngineMemoryReserveBytes) / 8);
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
        output << ' ' << record.comment;
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

auto detectProfile(std::span<const ReadRecord> records) -> Result<format::DatasetProfile> {
    if (records.empty()) {
        return format::DatasetProfile::kIllumina;
    }

    std::size_t ontHeaders = 0;
    std::size_t hifiHeaders = 0;
    std::size_t clrHeaders = 0;
    std::uint64_t totalBases = 0;
    std::size_t maxLength = 0;
    for (const auto& record : records) {
        const auto header = lowerCopy(record.id + " " + record.comment);
        ontHeaders += containsAny(header, {"runid=", " ch=", "channel="}) ? 1U : 0U;
        hifiHeaders += containsAny(header, {"/ccs", "hifi"}) ? 1U : 0U;
        const auto slashCount = static_cast<std::size_t>(std::ranges::count(record.id, '/'));
        clrHeaders += (containsAny(header, {"pacbio", "subread"}) ||
                       (!record.id.empty() && record.id.front() == 'm' && slashCount >= 2 &&
                        !header.contains("/ccs")))
            ? 1U
            : 0U;
        totalBases += record.sequence.size();
        maxLength = std::max(maxLength, record.sequence.size());
    }

    const auto majority = (records.size() + 1) / 2;
    if (hifiHeaders >= majority) {
        return format::DatasetProfile::kPacBioHiFi;
    }
    if (ontHeaders >= majority) {
        return format::DatasetProfile::kOnt;
    }
    if (clrHeaders >= majority) {
        return format::DatasetProfile::kPacBioClr;
    }
    const auto averageLength = totalBases / records.size();
    if (maxLength <= 1'000 && averageLength <= 500) {
        return format::DatasetProfile::kIllumina;
    }
    return makeError<format::DatasetProfile>(
        ErrorCode::kUsageError,
        "dataset profile is ambiguous; specify illumina, ont, pacbio-hifi, or pacbio-clr");
}

auto ArchiveEngine::compress(const CompressionRequest& request) const -> Result<OperationStats> {
    if (auto result = validateMemoryLimit(request.memoryLimitBytes); !result) {
        return makeError<OperationStats>(result.error());
    }
    if (request.targetFrameBytes == 0) {
        return makeError<OperationStats>(ErrorCode::kUsageError,
                                         "target frame size must be positive");
    }
    if (auto result = validateOutput(request.inputPath, request.outputPath, request.forceOverwrite);
        !result) {
        return makeError<OperationStats>(result.error());
    }
    if (request.paired() && request.inputPath == "-" && request.matePath == "-") {
        return makeError<OperationStats>(ErrorCode::kUsageError,
                                         "paired inputs cannot both use stdin");
    }

    auto primaryStream = io::openInputFile(request.inputPath);
    if (!primaryStream) {
        return makeError<OperationStats>(primaryStream.error());
    }
    io::FastqParser primary(**primaryStream);
    std::unique_ptr<std::istream> mateStreamPtr;
    std::optional<io::FastqParser> mate;
    if (request.paired()) {
        auto mateStream = io::openInputFile(request.matePath);
        if (!mateStream) {
            return makeError<OperationStats>(mateStream.error());
        }
        mateStreamPtr = std::move(*mateStream);
        mate.emplace(*mateStreamPtr);
    }

    std::vector<ReadRecord> sample;
    const auto maxSampleBytes = sampleLimitBytes(request.memoryLimitBytes);
    std::size_t sampledBases = 0;
    while (sample.size() < kProfileSampleMaxRecords && sampledBases < maxSampleBytes) {
        auto first = primary.readRecord();
        if (!first) {
            return makeError<OperationStats>(first.error());
        }
        if (!first->has_value()) {
            if (mate) {
                auto second = mate->readRecord();
                if (!second) {
                    return makeError<OperationStats>(second.error());
                }
                if (second->has_value()) {
                    return makeError<OperationStats>(ErrorCode::kFormatError,
                                                     "paired inputs have different record counts");
                }
            }
            break;
        }
        sampledBases += (*first)->sequence.size();
        sample.push_back(std::move(**first));
        if (mate) {
            auto second = mate->readRecord();
            if (!second) {
                return makeError<OperationStats>(second.error());
            }
            if (!second->has_value()) {
                return makeError<OperationStats>(ErrorCode::kFormatError,
                                                 "paired inputs have different record counts");
            }
            sampledBases += (*second)->sequence.size();
            sample.push_back(std::move(**second));
        }
    }

    auto resolvedProfile = request.profile.has_value()
        ? Result<format::DatasetProfile>(*request.profile)
        : detectProfile(sample);
    if (!resolvedProfile) {
        return makeError<OperationStats>(resolvedProfile.error());
    }

    OutputTransaction outputTransaction(request.outputPath, request.outputPath != "-");
    auto output = openOutput(outputTransaction.path());
    if (!output) {
        return makeError<OperationStats>(output.error());
    }
    format::ArchiveWriter writer(**output,
                                 {.profile = *resolvedProfile,
                                  .paired = request.paired(),
                                  .maxFrameBytes = maxFrameBytesFor(request.memoryLimitBytes),
                                  .memoryLimitBytes = request.memoryLimitBytes});
    pipeline::CompressPipeline pipelineEngine(targetFrameBytesFor(request), request.paired());
    std::istream* mateStream = mateStreamPtr ? mateStreamPtr.get() : nullptr;
    auto pipelineResult = pipelineEngine.run(
        **primaryStream, mateStream, std::span<const ReadRecord>{sample}, writer);
    if (!pipelineResult) {
        return makeError<OperationStats>(pipelineResult.error());
    }
    if (auto result = writer.finish(); !result) {
        return makeError<OperationStats>(result.error());
    }
    output->reset();
    if (auto result = outputTransaction.commit(request.forceOverwrite); !result) {
        return makeError<OperationStats>(result.error());
    }
    return toOperationStats(writer.metadata(), writer.stats(), true, pipelineResult->logicalBytes);
}

auto ArchiveEngine::decompress(const DecompressionRequest& request) const
    -> Result<OperationStats> {
    if (auto result = validateMemoryLimit(request.memoryLimitBytes); !result) {
        return makeError<OperationStats>(result.error());
    }
    if (auto result = validateOutput(request.inputPath, request.outputPath, request.forceOverwrite);
        !result) {
        return makeError<OperationStats>(result.error());
    }
    auto input = io::openInputFile(request.inputPath);
    if (!input) {
        return makeError<OperationStats>(input.error());
    }
    OutputTransaction outputTransaction(request.outputPath, request.outputPath != "-");
    auto output = openOutput(outputTransaction.path());
    if (!output) {
        return makeError<OperationStats>(output.error());
    }
    format::ArchiveReader reader(
        **input, maxFrameBytesFor(request.memoryLimitBytes), request.memoryLimitBytes);
    auto metadata = reader.open();
    if (!metadata) {
        return makeError<OperationStats>(metadata.error());
    }
    std::uint64_t logicalBytes = 0;
    while (true) {
        auto frame = reader.readFrame();
        if (!frame) {
            return makeError<OperationStats>(frame.error());
        }
        if (!frame->has_value()) {
            break;
        }
        for (const auto& record : **frame) {
            logicalBytes += canonicalFastqBytes(record);
            if (auto result = writeFastqRecord(**output, record); !result) {
                return makeError<OperationStats>(result.error());
            }
        }
    }
    (*output)->flush();
    if (!**output) {
        return makeError<OperationStats>(ErrorCode::kIOError, "failed to flush decompressed FASTQ");
    }
    output->reset();
    if (auto result = outputTransaction.commit(request.forceOverwrite); !result) {
        return makeError<OperationStats>(result.error());
    }
    return toOperationStats(*metadata, reader.stats(), false, logicalBytes);
}

auto ArchiveEngine::verify(const std::filesystem::path& inputPath,
                           std::size_t memoryLimitBytes) const -> Result<OperationStats> {
    if (auto result = validateMemoryLimit(memoryLimitBytes); !result) {
        return makeError<OperationStats>(result.error());
    }
    auto input = io::openInputFile(inputPath);
    if (!input) {
        return makeError<OperationStats>(input.error());
    }
    format::ArchiveReader reader(**input, maxFrameBytesFor(memoryLimitBytes), memoryLimitBytes);
    auto metadata = reader.open();
    if (!metadata) {
        return makeError<OperationStats>(metadata.error());
    }
    std::uint64_t logicalBytes = 0;
    while (true) {
        auto frame = reader.readFrame();
        if (!frame) {
            return makeError<OperationStats>(frame.error());
        }
        if (!frame->has_value()) {
            break;
        }
        for (const auto& record : **frame) {
            logicalBytes += canonicalFastqBytes(record);
        }
    }
    return toOperationStats(*metadata, reader.stats(), false, logicalBytes);
}

}  // namespace fqc::commands
