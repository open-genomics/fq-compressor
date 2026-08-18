#include "fqc/commands/archive_engine.h"

#include "fqc/commands/profile.h"

#include <cstddef>
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

[[nodiscard]] auto profileOf(std::string_view id, std::string_view comment, std::size_t bases)
    -> Result<format::DatasetProfile> {
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

}  // namespace fqc::commands::test
