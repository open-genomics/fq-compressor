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
#include <optional>
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
using fqc::pipeline::PipelineStats;

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
                               std::size_t targetFrameBytes,
                               PipelineStats* stats = nullptr) -> std::string {
    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    ParallelParsePipeline pipeline(
        input.path(), input.size(), targetFrameBytes, sampleEnd, workers);
    auto result = pipeline.run(sample, writer);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    if (stats != nullptr && result.has_value()) {
        *stats = *result;
    }
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

// findFirstRecordStart 现在返回 Result<optional>：错误即对齐区损坏（顺序路径
// 同样会失败），optional 为空即无记录起点。该辅助解包错误分支，让既有断言
// 保持"optional 语义"的可读性。
[[nodiscard]] auto alignedStart(std::istream& input,
                                std::uint64_t base) -> std::optional<std::uint64_t> {
    auto found = fqc::pipeline::findFirstRecordStart(input, base);
    EXPECT_TRUE(found.has_value()) << (found.has_value() ? "" : found.error().message);
    if (!found.has_value()) {
        return std::nullopt;
    }
    return *found;
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

    PipelineStats stats;
    const auto archive = runParallel(file, {}, 0, 4, 512, &stats);
    EXPECT_GT(stats.inFlightHighWater, 0U);
    EXPECT_LE(stats.inFlightHighWater, 10U);
    EXPECT_EQ(readAllRecords(archive), fqc::test::parseAllFastq(fastq));
}

TEST(ParallelParsePipelineTest, ParserFanoutDoesNotMultiplyEncoderPool) {
    const std::string fastq = makeFastq(2000);
    TempFastqFile file(fastq);

    PipelineStats stats;
    const auto archive = runParallel(file, {}, 0, 8, 512, &stats);

    EXPECT_EQ(stats.parserWorkers, 8U);
    EXPECT_EQ(stats.encoderWorkers, ParallelParsePipeline::kDefaultEncoderParallelism);
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

TEST(ParallelParsePipelineTest, StructuralMismatchInAlignmentZoneFailsLoudly) {
    // r1 的 quality 行长度与 sequence 不一致，其头部落在 worker 1 的对齐区：
    // 旧实现的假警报回退把 r1 静默跳过、在 r2 上重新对齐，产出缺少 r1 的
    // "合法"归档（verify 通过、数据丢失）。修复后必须与顺序路径一样报
    // kFormatError。
    std::string fastq = "@r0\n" + std::string(3000, 'A') + "\n+\n" + std::string(3000, 'I') + "\n";
    fastq += "@r1\n" + std::string(50, 'A') + "\n+\n" + std::string(60, 'I') + "\n";
    fastq += "@r2\n" + std::string(100, 'C') + "\n+\n" + std::string(100, 'I') + "\n";
    TempFastqFile file(fastq);

    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    ParallelParsePipeline pipeline(file.path(), file.size(), 1 << 20, 0, 2);
    auto result = pipeline.run({}, writer);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
}

TEST(ParallelParsePipelineTest, MissingPlusInAlignmentZoneFailsLoudly) {
    // r1 缺 '+' 行：旧实现读到的 l2 是 @r2 的头部，假警报回退后同样静默丢弃
    // r1 并在 r2 上完成对齐。修复后必须报 kFormatError。
    std::string fastq = "@r0\n" + std::string(3000, 'A') + "\n+\n" + std::string(3000, 'I') + "\n";
    fastq += "@r1\n" + std::string(50, 'A') + "\n";
    fastq += "@r2\n" + std::string(100, 'C') + "\n+\n" + std::string(100, 'I') + "\n";
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
    EXPECT_EQ(alignedStart(input, 0), 0U);
}

TEST(FindFirstRecordStartTest, MidRecordBaseAlignsToNextHeader) {
    const std::string fastq = makeFastq(3);
    // Land in the middle of the first record's sequence line. The stream
    // holds the full file and is seeked to the base (same usage as the
    // worker: stream position == baseOffset).
    const std::uint64_t base = fastq.find('\n') + 3;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    const auto found = alignedStart(input, base);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fastq[static_cast<std::size_t>(*found)], '@');
    // The aligned start must be the second record's header.
    EXPECT_EQ(fastq.substr(static_cast<std::size_t>(*found), 6), "@read_");
}

TEST(FindFirstRecordStartTest, AtQualityLineIsNotARecordStart) {
    const std::string fastq = makeFastq(3, 150, /*atQuality=*/true);
    // Point one byte into the '+' line of record 0: the discarded residue
    // starts with '+', so the '@' quality line that follows is recognized as
    // a quality-line residue (skipped) and scanning lands on record 1's header.
    const auto plusPos = fastq.find("\n+\n");
    const std::uint64_t residueBase = static_cast<std::uint64_t>(plusPos) + 1;
    ASSERT_EQ(fastq[static_cast<std::size_t>(residueBase)], '+');
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(residueBase));
    const auto found = alignedStart(input, residueBase);
    ASSERT_TRUE(found.has_value());
    // Must skip the false candidate and land on record 1's header.
    EXPECT_GT(*found, residueBase);
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
    EXPECT_EQ(alignedStart(input, badPos), badPos);
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
    EXPECT_EQ(alignedStart(input, base), truncPos);
}

TEST(FindFirstRecordStartTest, BareAtLineAtEofYieldsNullopt) {
    // Nothing after a final '@' line: it may be the file's last '@'-starting
    // quality line, which the previous chunk already parsed -- stay
    // conservative rather than failing a valid file spuriously.
    const std::string fastq = "@ok\nA\n+\n@\n";
    const std::uint64_t qualityStart = static_cast<std::uint64_t>(fastq.find("\n+\n")) + 3;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(qualityStart));
    EXPECT_FALSE(alignedStart(input, qualityStart).has_value());
}

TEST(FindFirstRecordStartTest, TruncatedTailYieldsNullopt) {
    const std::string fastq = makeFastq(1);
    // Start inside the only record: no complete record begins afterwards.
    const std::uint64_t base = fastq.find('\n') + 2;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    EXPECT_FALSE(alignedStart(input, base).has_value());
}

// BUG-2 regression (unit): findFirstRecordStart must NOT accept a position
// inside a header line (an '@' that is not at a line start) as a record
// start -- the previous worker already owns that record. It must skip the
// false candidate and land on the next true header.
TEST(FindFirstRecordStartTest, EmbeddedAtInsideHeaderIsNotARecordStart) {
    const std::string fastq = "@ok\nACGT\n+\nIIII\n@r@2\nCGTA\n+\nJJJJ\n@tail\nGGCC\n+\nHHHH\n";
    const auto embedded = static_cast<std::uint64_t>(fastq.find("@r@2")) + 2;
    ASSERT_EQ(fastq[static_cast<std::size_t>(embedded)], '@');
    ASSERT_NE(fastq[static_cast<std::size_t>(embedded) - 1], '\n');
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(embedded));
    const auto found = alignedStart(input, embedded);
    ASSERT_TRUE(found.has_value()) << "scan must skip the mid-line '@' and find @tail";
    EXPECT_NE(*found, embedded);
    EXPECT_EQ(fastq.substr(static_cast<std::size_t>(*found), 5), "@tail");
}

// =============================================================================
// BUG-3 regression: 对齐区内的结构畸形不得被静默跳过
// =============================================================================

TEST(FindFirstRecordStartTest, QualityLengthMismatchCandidateIsReturned) {
    // r1 的 quality 行长度与 sequence 不一致：结构拒绝且非质量行残段（其前无 '+'
    // 锚点）→ 扫描必须返回 r1，让 worker 的 parser 报与顺序路径相同的 kFormatError，
    // 而不是跳过它命中 r2（静默丢）。
    const std::string fastq = "@r0\nACGT\n+\nIIII\n@r1\nACG\n+\nIIII\n@r2\nACGT\n+\nIIII\n";
    const std::uint64_t r1Pos = static_cast<std::uint64_t>(fastq.find("@r1"));
    const std::uint64_t base = 2;  // 落在 "@r0" 头部行中段
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    auto found = fqc::pipeline::findFirstRecordStart(input, base);
    ASSERT_TRUE(found.has_value()) << (found.has_value() ? "" : found.error().message);
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(*found, r1Pos);
}

TEST(FindFirstRecordStartTest, MissingPlusLineCandidateIsReturned) {
    // 缺 '+' 行的记录：@r1 结构拒绝（其第三行是 @r2 的头，非 '+'）且非质量行残段
    // → 返回 @r1 让 parser 报错，而不是静默丢弃 r1、把 @r2 当合法记录。
    const std::string fastq = "@r0\nACGT\n+\nIIII\n@r1\nACGT\n@r2\nACGT\n+\nIIII\n";
    const std::uint64_t r1Pos = static_cast<std::uint64_t>(fastq.find("@r1"));
    const std::uint64_t base = 2;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    auto found = fqc::pipeline::findFirstRecordStart(input, base);
    ASSERT_TRUE(found.has_value()) << (found.has_value() ? "" : found.error().message);
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(*found, r1Pos);
}

TEST(FindFirstRecordStartTest, BlankLinesInAlignmentZoneAreSkipped) {
    // 记录之间的空行与顺序路径一样跳过：残段（'+' + quality）之后的空行不能
    // 阻止对齐到 @next。
    const std::string fastq = "@ok\nACGT\n+\nIIII\n\n\n@next\nCGTA\n+\nJJJJ\n";
    const std::uint64_t base = static_cast<std::uint64_t>(fastq.find('\n')) + 2;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    const auto found = alignedStart(input, base);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, static_cast<std::uint64_t>(fastq.find("@next")));
}

TEST(FindFirstRecordStartTest, CorruptTailWithoutAtYieldsError) {
    // EOF 前的 2 个非空行既不是可行残段（[+'-',qual] 要求首行以 '+' 开头），
    // 也没有可交给 parser 的 '@' 候选：顺序路径会以 "expected '@'" 拒绝，
    // 扫描必须同样报错而不是返回空（静默截断）。
    const std::string fastq = "@ok\nACGT\n+\nIIII\ngarbage1\ngarbage2\n";
    const std::uint64_t base = static_cast<std::uint64_t>(fastq.find('\n')) + 2;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    auto found = fqc::pipeline::findFirstRecordStart(input, base);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().code, ErrorCode::kFormatError);
}

TEST(FindFirstRecordStartTest, OversizedLineInAlignmentZoneYieldsError) {
    // 行长上限：对齐扫描与 parser 使用同一上限，超长行以 kFormatError 拒绝，
    // 而不是把整行读进内存后再静默跳过。
    const std::string fastq = "@ok\nACGT\n+\nIIII\n@big\n" + std::string(100, 'A') + "\n+\n" +
        std::string(100, 'I') + "\n";
    const std::uint64_t base = static_cast<std::uint64_t>(fastq.find('\n')) + 2;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    auto found = fqc::pipeline::findFirstRecordStart(input, base, /*maxLineBytes=*/32);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().code, ErrorCode::kFormatError);
}

// BUG-2 regression (integration): with the chunk boundary landing exactly on
// an '@' embedded inside a header line, the parallel pipeline must not inject
// a phantom record. The decoded record stream must equal the reference.
TEST(ParallelParsePipelineTest, EmbeddedAtOnChunkBoundaryDoesNotDuplicateRecords) {
    for (std::size_t pad = 4; pad <= 64; ++pad) {
        const std::string content = "@pad\n" + std::string(pad, 'A') + "\n+\n" +
            std::string(pad, 'I') + "\n@r@2\nCGTA\n+\nJJJJ\n@tail\nGGCC\n+\nHHHH\n";
        const auto recPos = static_cast<std::uint64_t>(content.find("\n@r@2")) + 1;
        const uint64_t embedded = recPos + 2;
        const uint64_t n = static_cast<std::uint64_t>(content.size());
        // Two workers: chunk 1 begins at ceil(n/2). Look for a pad length that
        // puts the boundary exactly on the embedded '@'.
        const uint64_t boundary = (n + 1) / 2;
        if (boundary != embedded || embedded == 0 || recPos >= boundary) {
            continue;
        }
        TempFastqFile input(content);
        const auto archive = runParallel(input, {}, 0, 2, 4096);
        const auto got = readAllRecords(archive);
        const auto want = fqc::test::parseAllFastq(content);
        ASSERT_EQ(got.size(), want.size())
            << "phantom/split record at boundary=" << boundary << " pad=" << pad;
        EXPECT_EQ(got, want);
        return;  // one demonstration is enough
    }
    FAIL() << "test setup failed: no pad length hit the embedded '@' boundary";
}

// 对抗性回归：畸形记录只有 id 行（缺 body）、下一条记录头被吞时（@r1\n@r2\n…），
// 对齐扫描若把 @r2 当合法记录返回会静默丢掉 @r1——顺序路径对 @r1 报 "expected '+'"。
TEST(FindFirstRecordStartTest, MalformedEmptyBodyCandidateIsReturned) {
    const std::string fastq = "@ok\nACGT\n+\nIIII\n@r1\n@r2\nCGTA\n+\nJJJJ\n@tail\nGGCC\n+\nHHHH\n";
    // base 落在 @ok 的 qual 行中段：残段丢弃后窗口第一行是 @r1（对齐区内）
    const std::uint64_t base = static_cast<std::uint64_t>(fastq.find("\n+\n")) + 3 + 2;
    std::istringstream input(fastq);
    input.seekg(static_cast<std::streamoff>(base));
    auto found = fqc::pipeline::findFirstRecordStart(input, base);
    ASSERT_TRUE(found.has_value()) << (found.has_value() ? "" : found.error().message);
    ASSERT_TRUE(found->has_value());
    // 必须返回 @r1（让 parser 报错），而不是跳过它命中 @r2
    EXPECT_EQ(fastq.substr(static_cast<std::size_t>(**found), 3), "@r1");
}
