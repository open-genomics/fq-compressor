#include "fqc/commands/archive_engine.h"

#include "fqc/commands/profile.h"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "support.h"

#include <gtest/gtest.h>

namespace fqc::commands::test {

namespace {

constexpr std::string_view kShortFastq =
    "@short_1 1:N:0:ACGT\nACGTNacgt\n+\n!#IJKLMNO\n"
    "@short_2 2:N:0:TGCA\nTGCARYSWK\n+\nJKLMNOPQR\n";

[[nodiscard]] auto profileOf(std::string_view id,
                             std::string_view comment,
                             std::size_t bases) -> Result<format::DatasetProfile> {
    return detectProfile(std::vector{ReadRecord{
        std::string(id), std::string(comment), std::string(bases, 'A'), std::string(bases, 'I')}});
}

class ArchiveEngineTest : public ::testing::Test {
protected:
    fqc::test::TempDir temp_;
};

}  // namespace

TEST_F(ArchiveEngineTest, CompressesAndDecompressesCanonicalFastq) {
    temp_.writeFile("reads.fastq", kShortFastq);
    ArchiveEngine engine;

    auto compressed = engine.compress({.inputPath = temp_.path() / "reads.fastq",
                                       .matePath = {},
                                       .outputPath = temp_.path() / "reads.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .targetFrameBytes = 64,
                                       .forceOverwrite = true});
    ASSERT_TRUE(compressed) << compressed.error().message;
    EXPECT_EQ(compressed->recordCount, 2U);
    EXPECT_GE(compressed->frameCount, 1U);

    auto decompressed = engine.decompress({.inputPath = temp_.path() / "reads.fqc",
                                           .outputPath = temp_.path() / "restored.fastq",
                                           .memoryLimitBytes = 64 * 1024 * 1024,
                                           .forceOverwrite = true});
    ASSERT_TRUE(decompressed);
    EXPECT_EQ(temp_.readFile("restored.fastq"), kShortFastq);

    auto verified = engine.verify(temp_.path() / "reads.fqc", 64 * 1024 * 1024);
    ASSERT_TRUE(verified);
    EXPECT_EQ(verified->recordCount, 2U);
}

TEST_F(ArchiveEngineTest, InterleavesPairedFilesAndKeepsPairsAtomic) {
    temp_.writeFile("r1.fastq", "@pair/1\nACGT\n+\nIIII\n");
    temp_.writeFile("r2.fastq", "@pair/2\nTGCA\n+\nJJJJ\n");
    ArchiveEngine engine;

    auto compressed = engine.compress({.inputPath = temp_.path() / "r1.fastq",
                                       .matePath = temp_.path() / "r2.fastq",
                                       .outputPath = temp_.path() / "paired.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .targetFrameBytes = 1,
                                       .forceOverwrite = true});
    ASSERT_TRUE(compressed);
    EXPECT_TRUE(compressed->paired);
    EXPECT_EQ(compressed->recordCount, 2U);

    auto decompressed = engine.decompress({.inputPath = temp_.path() / "paired.fqc",
                                           .outputPath = temp_.path() / "paired.fastq",
                                           .memoryLimitBytes = 64 * 1024 * 1024,
                                           .forceOverwrite = true});
    ASSERT_TRUE(decompressed);
    EXPECT_EQ(temp_.readFile("paired.fastq"), "@pair/1\nACGT\n+\nIIII\n@pair/2\nTGCA\n+\nJJJJ\n");
}

TEST_F(ArchiveEngineTest, DetectsShortReadsAsIllumina) {
    ASSERT_EQ(profileOf("read", "", 4).value(), format::DatasetProfile::kIllumina);
}

TEST_F(ArchiveEngineTest, DetectsNativeOntHeader) {
    ASSERT_EQ(profileOf("abc", "runid=123 ch=7", 2'000).value(), format::DatasetProfile::kOnt);
}

TEST_F(ArchiveEngineTest, DetectsPacBioHifiHeader) {
    ASSERT_EQ(profileOf("m64011_220101_010101/42/ccs", "", 2'000).value(),
              format::DatasetProfile::kPacBioHiFi);
}

TEST_F(ArchiveEngineTest, DetectsPacBioClrHeader) {
    ASSERT_EQ(profileOf("m64011_220101_010101/42/0_2000", "", 2'000).value(),
              format::DatasetProfile::kPacBioClr);
}

TEST_F(ArchiveEngineTest, RejectsUnmarkedLongReads) {
    auto ambiguous = profileOf("unknown", "", 2'000);
    ASSERT_FALSE(ambiguous);
    EXPECT_EQ(ambiguous.error().code, ErrorCode::kUsageError);
}

TEST_F(ArchiveEngineTest, DetectsEnaLongReadAccessionAsOnt) {
    ASSERT_EQ(profileOf("DRR171398.1", "1/1", 16'340).value(), format::DatasetProfile::kOnt);
    ASSERT_EQ(profileOf("ERR1234567.1", "1/1", 2'000).value(), format::DatasetProfile::kOnt);
}

TEST_F(ArchiveEngineTest, KeepsShortEnaAccessionAsIllumina) {
    ASSERT_EQ(profileOf("SRR2962693.1", "1/1", 126).value(), format::DatasetProfile::kIllumina);
}

TEST_F(ArchiveEngineTest, PrefersHifiMarkerOverEnaAccession) {
    ASSERT_EQ(profileOf("SRR2962693.1", "/ccs", 2'000).value(),
              format::DatasetProfile::kPacBioHiFi);
}

TEST_F(ArchiveEngineTest, RejectsPairedCountMismatchAndTinyMemoryLimit) {
    temp_.writeFile("r1.fastq", "@a/1\nACGT\n+\nIIII\n@b/1\nACGT\n+\nIIII\n");
    temp_.writeFile("r2.fastq", "@a/2\nTGCA\n+\nJJJJ\n");
    ArchiveEngine engine;

    auto mismatch = engine.compress({.inputPath = temp_.path() / "r1.fastq",
                                     .matePath = temp_.path() / "r2.fastq",
                                     .outputPath = temp_.path() / "bad.fqc",
                                     .profile = format::DatasetProfile::kIllumina,
                                     .memoryLimitBytes = 64 * 1024 * 1024,
                                     .forceOverwrite = true});
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code, ErrorCode::kFormatError);

    auto tinyMemory = engine.compress({.inputPath = temp_.path() / "r1.fastq",
                                       .matePath = {},
                                       .outputPath = temp_.path() / "tiny.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 1024,
                                       .forceOverwrite = true});
    ASSERT_FALSE(tinyMemory);
    EXPECT_EQ(tinyMemory.error().code, ErrorCode::kUsageError);
}

// BUG-1 regression: a header ending in exactly one trailing space must
// round-trip byte-identically (lossless constraint), not silently drop it.
TEST_F(ArchiveEngineTest, RoundTripsHeaderWithTrailingSpaceByteIdentically) {
    const std::string input = "@r1 \nACGT\n+\nIIII\n@r2 x\nTGCA\n+\nJJJJ\n";
    temp_.writeFile("ts.fastq", input);
    ArchiveEngine engine;

    auto compressed = engine.compress({.inputPath = temp_.path() / "ts.fastq",
                                       .matePath = {},
                                       .outputPath = temp_.path() / "ts.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .forceOverwrite = true});
    ASSERT_TRUE(compressed) << compressed.error().message;

    auto decompressed = engine.decompress({.inputPath = temp_.path() / "ts.fqc",
                                           .outputPath = temp_.path() / "ts.out.fastq",
                                           .memoryLimitBytes = 64 * 1024 * 1024,
                                           .forceOverwrite = true});
    ASSERT_TRUE(decompressed) << decompressed.error().message;
    EXPECT_EQ(temp_.readFile("ts.out.fastq"), input);
}

// BUG-1 regression: genuine trailing \r inside sequence/quality must survive
// (only the single CRLF line-ending \r is stripped).
TEST_F(ArchiveEngineTest, RejectsGenuineTrailingCarriageReturnsLoudly) {
    // "SEQ\r\r\n": the last \r pairs with \n (CRLF line ending, stripped);
    // the first is NOT a line-ending artifact. The archive format rejects \r
    // inside sequence/quality (fail-closed), so the parser keeping one \r must
    // make encode fail loudly instead of silently stripping both and
    // round-tripping different data.
    const std::string input = "@r1\nSEQ\r\r\n+\nIII\r\r\n";
    temp_.writeFile("cr.fastq", input);

    ArchiveEngine engine;
    auto compressed = engine.compress({.inputPath = temp_.path() / "cr.fastq",
                                       .matePath = {},
                                       .outputPath = temp_.path() / "cr.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .forceOverwrite = true});
    ASSERT_FALSE(compressed);
    EXPECT_EQ(compressed.error().code, ErrorCode::kUsageError);
}

// Unsupported compression formats (bzip2/xz/zstd) must fail closed: even
// though the magic is recognized, the engine rejects them rather than
// misinterpreting the compressed bytes as FASTQ.
TEST_F(ArchiveEngineTest, RejectsUnsupportedCompressionFormatsFailClosed) {
    // magic 字节序列按实际长度构造（含 \x00，C 字符串语义的 strlen 会截断，
    // 必须用显式长度）；detectCompressionFormat 只认前几个 magic 字节。
    const std::string bzip2("BZh91AY&SY\x1a\x00\x00\x00", 13);
    const std::string zstd("\x28\xb5\x2f\xfd\x00\x00\x00\x00", 8);
    const std::string xz("\xfd\x37\x7a\x58\x5a\x00", 6);
    temp_.writeFile("input.bz2", bzip2);
    temp_.writeFile("input.zst", zstd);
    temp_.writeFile("input.xz", xz);

    ArchiveEngine engine;
    for (const char* name : {"input.bz2", "input.zst", "input.xz"}) {
        auto result = engine.compress({.inputPath = temp_.path() / name,
                                       .matePath = {},
                                       .outputPath = temp_.path() / (std::string(name) + ".fqc"),
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .forceOverwrite = true});
        ASSERT_FALSE(result) << name;
        EXPECT_EQ(result.error().code, ErrorCode::kIOError) << name;
    }
}

// A mate file that ends while the primary still has records is caught before
// any output archive is created (fail-closed, nothing partial left behind).
TEST_F(ArchiveEngineTest, RejectsPrimaryLongerThanMateWithoutOutput) {
    temp_.writeFile("r1.fastq", "@a/1\nACGT\n+\nIIII\n@b/1\nTGCA\n+\nIIII\n");
    temp_.writeFile("r2.fastq", "@a/2\nTGCA\n+\nJJJJ\n");

    ArchiveEngine engine;
    auto result = engine.compress({.inputPath = temp_.path() / "r1.fastq",
                                   .matePath = temp_.path() / "r2.fastq",
                                   .outputPath = temp_.path() / "mismatch.fqc",
                                   .profile = format::DatasetProfile::kIllumina,
                                   .memoryLimitBytes = 64 * 1024 * 1024,
                                   .forceOverwrite = true});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
    EXPECT_FALSE(std::filesystem::exists(temp_.path() / "mismatch.fqc"));
}

// gzip-compressed input via a file path (not stdin) must be read transparently
// end to end; the input file itself is never the output target.
TEST_F(ArchiveEngineTest, CompressesGzipFileInputTransparently) {
    temp_.writeFile("reads.fastq", kShortFastq);
    // 压缩输入副本为 .gz 文件。CLI 二进制调用 gzip；测试直接构造 gzip 数据太重，
    // 这里用一个已知的 gzip 头 + 存储块的最小 gzip 流是不可靠的，因此调用系统
    // gzip（与 compressed_stream_test 相同的依赖）。
    // 但为保持测试环境独立性，这里用 archive 无关的最小 gzip 生成：
    // 直接调用系统 gzip 命令。
    const auto gzipPath = temp_.path() / "reads.fastq.gz";
    const auto gzipCmd =
        "gzip -c " + (temp_.path() / "reads.fastq").string() + " > " + gzipPath.string();
    ASSERT_EQ(std::system(gzipCmd.c_str()), 0) << "system gzip failed";

    ArchiveEngine engine;
    auto compressed = engine.compress({.inputPath = gzipPath,
                                       .matePath = {},
                                       .outputPath = temp_.path() / "gzip.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .forceOverwrite = true});
    ASSERT_TRUE(compressed) << compressed.error().message;
    EXPECT_EQ(compressed->recordCount, 2U);

    auto decompressed = engine.decompress({.inputPath = temp_.path() / "gzip.fqc",
                                           .outputPath = temp_.path() / "restored.fastq",
                                           .memoryLimitBytes = 64 * 1024 * 1024,
                                           .forceOverwrite = true});
    ASSERT_TRUE(decompressed);
    EXPECT_EQ(temp_.readFile("restored.fastq"), kShortFastq);
}

// verify is a pure reader: on a corrupt archive it fails without creating or
// touching any output file.
TEST_F(ArchiveEngineTest, VerifyFailsWithoutWritingOutput) {
    temp_.writeFile("reads.fastq", kShortFastq);
    ArchiveEngine engine;
    auto compressed = engine.compress({.inputPath = temp_.path() / "reads.fastq",
                                       .matePath = {},
                                       .outputPath = temp_.path() / "reads.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .forceOverwrite = true});
    ASSERT_TRUE(compressed);

    // 篡改 payload 区（全局头 32 字节之后），使校验和/解码失败。
    auto bytes = temp_.readFile("reads.fqc");
    bytes[32] ^= 0x01;
    temp_.writeFile("tampered.fqc", bytes);

    auto verified = engine.verify(temp_.path() / "tampered.fqc", 64 * 1024 * 1024);
    EXPECT_FALSE(verified);
}

}  // namespace fqc::commands::test
