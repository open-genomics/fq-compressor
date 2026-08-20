// =============================================================================
// fq-compressor - Parallel Parse Pipeline Integration Tests
// =============================================================================

#include "fqc/pipeline/parallel_parse_pipeline.h"

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/compress_pipeline.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support.h"

#include <gtest/gtest.h>

using fqc::ErrorCode;
using fqc::ReadRecord;
using fqc::format::ArchiveReader;
using fqc::format::ArchiveWriter;
using fqc::format::DatasetProfile;
using fqc::pipeline::CompressPipeline;
using fqc::pipeline::ParallelParsePipeline;

namespace {

[[nodiscard]] auto makeFastq(int count,
                             int sequenceLength = 150,
                             bool atQuality = false) -> std::string {
    std::string fastq;
    for (int i = 0; i < count; ++i) {
        fastq += "@read_" + std::to_string(i) + " comment\n";
        fastq += std::string(static_cast<std::size_t>(sequenceLength), 'A');
        fastq += "\n+\n";
        // Adversarial mode: quality starts with '@' (Q31), the classic false
        // record-start candidate that must not fool boundary alignment.
        fastq += atQuality ? "@" + std::string(static_cast<std::size_t>(sequenceLength - 1), 'I')
                           : std::string(static_cast<std::size_t>(sequenceLength), 'I');
        fastq += "\n";
    }
    return fastq;
}

class TempFastqFile {
public:
    explicit TempFastqFile(const std::string& content) {
        static std::atomic<int> sequence{0};
        path_ = std::filesystem::temp_directory_path() /
            ("fqc_h_test_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(sequence.fetch_add(1)) + ".fastq");
        std::ofstream file(path_, std::ios::binary);
        file << content;
        size_ = content.size();
    }

    ~TempFastqFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

    [[nodiscard]] auto size() const -> std::uint64_t {
        return size_;
    }

private:
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
};

[[nodiscard]] auto runParallel(const TempFastqFile& input,
                               std::span<const ReadRecord> sample,
                               std::uint64_t sampleEnd,
                               std::size_t workers,
                               std::size_t targetFrameBytes) -> std::string {
    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    ParallelParsePipeline pipeline(
        input.path(), input.size(), targetFrameBytes, sampleEnd, workers);
    auto result = pipeline.run(sample, writer);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(writer.finish());
    return output.str();
}

[[nodiscard]] auto runSequential(std::istream& input,
                                 std::span<const ReadRecord> sample,
                                 std::size_t targetFrameBytes) -> std::string {
    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    CompressPipeline pipeline(targetFrameBytes);
    auto result = pipeline.run(input, nullptr, sample, writer);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(writer.finish());
    return output.str();
}

[[nodiscard]] auto readAllRecords(const std::string& archive) -> std::vector<ReadRecord> {
    std::istringstream input(archive, std::ios::binary);
    ArchiveReader reader(input);
    EXPECT_TRUE(reader.open().has_value());
    std::vector<ReadRecord> records;
    while (true) {
        auto frame = reader.readFrame();
        EXPECT_TRUE(frame.has_value());
        if (!frame->has_value()) {
            break;
        }
        records.insert(records.end(),
                       std::make_move_iterator((*frame)->begin()),
                       std::make_move_iterator((*frame)->end()));
    }
    return records;
}

}  // namespace

// The stage-H gem: with a single chunk worker the parallel pipeline's framing
// matches the sequential pipeline exactly -- byte-identical archives.
TEST(ParallelParsePipelineTest, SingleWorkerIsByteIdenticalToSequential) {
    const std::string fastq = makeFastq(500);
    TempFastqFile file(fastq);

    std::istringstream sequentialInput(fastq);
    const auto sequential = runSequential(sequentialInput, {}, 128);
    const auto parallel = runParallel(file, {}, 0, 1, 128);

    EXPECT_EQ(parallel, sequential);
}

// Same gate with a profile sample: the sample seeds worker 0's accumulator,
// so framing stays continuous across the sample boundary.
TEST(ParallelParsePipelineTest, SampledSingleWorkerIsByteIdenticalToSequential) {
    const std::string fastq = makeFastq(500);
    TempFastqFile file(fastq);

    // Sample 50 records on the "main thread" and note the exact byte offset.
    std::istringstream samplingStream(fastq);
    fqc::io::FastqParser sampler(samplingStream);
    std::vector<ReadRecord> sample;
    for (int i = 0; i < 50; ++i) {
        auto record = sampler.readRecord();
        ASSERT_TRUE(record.has_value() && record->has_value());
        sample.push_back(std::move(**record));
    }
    const std::uint64_t sampleEnd = sampler.bytesConsumed();

    // Sequential: continue parsing from the byte offset the sampler reached.
    std::istringstream sequentialInput(fastq.substr(static_cast<std::size_t>(sampleEnd)));
    const auto sequential = runSequential(sequentialInput, sample, 128);
    const auto parallel = runParallel(file, sample, sampleEnd, 1, 128);

    EXPECT_EQ(parallel, sequential);
    EXPECT_EQ(readAllRecords(parallel), fqc::test::parseAllFastq(fastq));
}

TEST(ParallelParsePipelineTest, MultiWorkerPreservesRecordOrderAndContent) {
    const std::string fastq = makeFastq(2000);
    TempFastqFile file(fastq);

    const auto archive = runParallel(file, {}, 0, 4, 512);
    EXPECT_EQ(readAllRecords(archive), fqc::test::parseAllFastq(fastq));
}

TEST(ParallelParsePipelineTest, AdversarialAtQualityLinesRoundTrip) {
    // Quality lines start with '@': a naive "line starts with '@'" splitter
    // would misalign; the 4-line structural check must skip them.
    const std::string fastq = makeFastq(1200, 150, /*atQuality=*/true);
    TempFastqFile file(fastq);

    const auto archive = runParallel(file, {}, 0, 4, 256);
    EXPECT_EQ(readAllRecords(archive), fqc::test::parseAllFastq(fastq));
}

TEST(ParallelParsePipelineTest, SampleCoveringWholeFileYieldsSampleOnlyArchive) {
    const std::string fastq = makeFastq(40);
    TempFastqFile file(fastq);

    const auto sample = fqc::test::parseAllFastq(fastq);
    const auto archive = runParallel(file, sample, file.size(), 4, 128);
    EXPECT_EQ(readAllRecords(archive), sample);
}

TEST(ParallelParsePipelineTest, EmptyFileProducesEmptyArchive) {
    TempFastqFile file("");
    const auto archive = runParallel(file, {}, 0, 4, 128);
    EXPECT_TRUE(readAllRecords(archive).empty());
}

TEST(ParallelParsePipelineTest, WorkerBeyondFileSizeEmitsOnlyMarker) {
    // Tiny file, many workers: chunks past EOF produce zero frames but the
    // ordering protocol must still complete.
    const std::string fastq = makeFastq(3);
    TempFastqFile file(fastq);

    const auto archive = runParallel(file, {}, 0, 8, 1 << 20);
    EXPECT_EQ(readAllRecords(archive), fqc::test::parseAllFastq(fastq));
}

TEST(ParallelParsePipelineTest, MalformedRecordInAlignmentZoneFailsLoudly) {
    // r1 carries a non-IUPAC sequence. With two workers the chunk boundary
    // lands inside r0, so r1's header sits in worker 1's alignment zone: the
    // scan must accept it (structure mirrors the parser, content validation
    // stays in encodeFrame) so the run fails exactly like the sequential
    // path -- the old IUPAC pre-check silently skipped the record.
    std::string fastq = "@r0\n" + std::string(3000, 'A') + "\n+\n" + std::string(3000, 'I') + "\n";
    fastq += "@r1\n" + std::string(50, 'A') + "Z" + std::string(49, 'A') + "\n+\n" +
        std::string(100, 'I') + "\n";
    fastq += "@r2\n" + std::string(100, 'C') + "\n+\n" + std::string(100, 'I') + "\n";
    TempFastqFile file(fastq);

    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    ParallelParsePipeline pipeline(file.path(), file.size(), 1 << 20, 0, 2);
    auto result = pipeline.run({}, writer);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kUsageError);
}

TEST(ParallelParsePipelineTest, TruncatedTailInAlignmentZoneFailsLoudly) {
    // A record cut mid-body at EOF, header inside the last chunk's alignment
    // zone: the scan returns it and the parser raises the same unexpected-EOF
    // error the sequential path would, instead of dropping the tail.
    std::string fastq = "@r0\n" + std::string(3000, 'A') + "\n+\n" + std::string(3000, 'I') + "\n";
    fastq += "@trunc\n" + std::string(30, 'A');  // no '+' line: cut mid-record
    TempFastqFile file(fastq);

    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    ParallelParsePipeline pipeline(file.path(), file.size(), 1 << 20, 0, 2);
    auto result = pipeline.run({}, writer);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
}

// =============================================================================
// findFirstRecordStart unit tests
// =============================================================================

TEST(FindFirstRecordStartTest, HeaderAtBaseIsReturned) {
    const std::string fastq = makeFastq(3);
    std::istringstream input(fastq);
    EXPECT_EQ(fqc::pipeline::findFirstRecordStart(input, 0), 0U);
}

TEST(FindFirstRecordStartTest, MidRecordBaseAlignsToNextHeader) {
    const std::string fastq = makeFastq(3);
    // Land in the middle of the first record's sequence line. The stream
    // holds the full file and is seeked to the base (same usage as the
    // worker: stream position == baseOffset).
    const std::uint64_t base = fastq.find('\n') + 3;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    const auto found = fqc::pipeline::findFirstRecordStart(input, base);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fastq[static_cast<std::size_t>(*found)], '@');
    // The aligned start must be the second record's header.
    EXPECT_EQ(fastq.substr(static_cast<std::size_t>(*found), 6), "@read_");
}

TEST(FindFirstRecordStartTest, AtQualityLineIsNotARecordStart) {
    const std::string fastq = makeFastq(3, 150, /*atQuality=*/true);
    // Point exactly at the '@' quality line of record 0.
    const auto plusPos = fastq.find("\n+\n");
    const std::uint64_t qualityStart = static_cast<std::uint64_t>(plusPos) + 3;
    ASSERT_EQ(fastq[static_cast<std::size_t>(qualityStart)], '@');
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(qualityStart));
    const auto found = fqc::pipeline::findFirstRecordStart(input, qualityStart);
    ASSERT_TRUE(found.has_value());
    // Must skip the false candidate and land on record 1's header.
    EXPECT_GT(*found, qualityStart);
    EXPECT_EQ(fastq.substr(static_cast<std::size_t>(*found), 7), "@read_1");
}

TEST(FindFirstRecordStartTest, MalformedSequenceCandidateIsAccepted) {
    // Content validation lives in encodeFrame, not in alignment: a record
    // with a non-IUPAC sequence must still be found, so the pipeline fails
    // as loudly as the sequential path instead of skipping the record.
    const std::string fastq = "@ok\nACGT\n+\nIIII\n@bad\nACZT\n+\nIIII\n";
    const auto badPos = static_cast<std::uint64_t>(fastq.find("@bad"));
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(badPos));
    EXPECT_EQ(fqc::pipeline::findFirstRecordStart(input, badPos), badPos);
}

TEST(FindFirstRecordStartTest, TruncatedMidBodyCandidateIsReturned) {
    // Header + partial sequence + EOF: the record starts here, so return its
    // offset and let the parser raise the sequential path's error instead of
    // dropping the tail silently.
    const std::string fastq = "@ok\nACGTACGT\n+\nIIIIIIII\n@trunc\nACG";
    const std::uint64_t truncPos = static_cast<std::uint64_t>(fastq.find("@trunc"));
    // Start the scan inside record 0 so it walks into the truncated record.
    const std::uint64_t base = static_cast<std::uint64_t>(fastq.find('\n')) + 3;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    EXPECT_EQ(fqc::pipeline::findFirstRecordStart(input, base), truncPos);
}

TEST(FindFirstRecordStartTest, BareAtLineAtEofYieldsNullopt) {
    // Nothing after a final '@' line: it may be the file's last '@'-starting
    // quality line, which the previous chunk already parsed -- stay
    // conservative rather than failing a valid file spuriously.
    const std::string fastq = "@ok\nA\n+\n@\n";
    const std::uint64_t qualityStart = static_cast<std::uint64_t>(fastq.find("\n+\n")) + 3;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(qualityStart));
    EXPECT_FALSE(fqc::pipeline::findFirstRecordStart(input, qualityStart).has_value());
}

TEST(FindFirstRecordStartTest, TruncatedTailYieldsNullopt) {
    const std::string fastq = makeFastq(1);
    // Start inside the only record: no complete record begins afterwards.
    const std::uint64_t base = fastq.find('\n') + 2;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    EXPECT_FALSE(fqc::pipeline::findFirstRecordStart(input, base).has_value());
}
