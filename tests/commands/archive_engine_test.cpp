#include "fqc/commands/archive_engine.h"

#include "fqc/io/fastq_parser.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace fqc::commands::test {

namespace {

constexpr std::string_view kShortFastq =
    "@short_1 1:N:0:ACGT\nACGTNacgt\n+\n!#IJKLMNO\n"
    "@short_2 2:N:0:TGCA\nTGCARYSWK\n+\nJKLMNOPQR\n";

class ArchiveEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = std::filesystem::temp_directory_path() /
            ("fqc_engine_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tempDir_);
    }

    void writeFile(const std::filesystem::path& path, std::string_view content) {
        std::ofstream file(path, std::ios::binary);
        file << content;
    }

    [[nodiscard]] auto readFileContent(const std::filesystem::path& path) -> std::string {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::filesystem::path tempDir_;
};

}  // namespace

TEST_F(ArchiveEngineTest, CompressesAndDecompressesCanonicalFastq) {
    writeFile(tempDir_ / "reads.fastq", kShortFastq);
    ArchiveEngine engine;

    auto compressed = engine.compress({.inputPath = tempDir_ / "reads.fastq",
                                       .matePath = {},
                                       .outputPath = tempDir_ / "reads.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .targetFrameBytes = 64,
                                       .forceOverwrite = true});
    ASSERT_TRUE(compressed) << compressed.error().message;
    EXPECT_EQ(compressed->recordCount, 2U);
    EXPECT_GE(compressed->frameCount, 1U);

    auto decompressed = engine.decompress({.inputPath = tempDir_ / "reads.fqc",
                                           .outputPath = tempDir_ / "restored.fastq",
                                           .memoryLimitBytes = 64 * 1024 * 1024,
                                           .forceOverwrite = true});
    ASSERT_TRUE(decompressed);
    EXPECT_EQ(readFileContent(tempDir_ / "restored.fastq"), kShortFastq);

    auto verified = engine.verify(tempDir_ / "reads.fqc", 64 * 1024 * 1024);
    ASSERT_TRUE(verified);
    EXPECT_EQ(verified->recordCount, 2U);
}

TEST_F(ArchiveEngineTest, InterleavesPairedFilesAndKeepsPairsAtomic) {
    writeFile(tempDir_ / "r1.fastq", "@pair/1\nACGT\n+\nIIII\n");
    writeFile(tempDir_ / "r2.fastq", "@pair/2\nTGCA\n+\nJJJJ\n");
    ArchiveEngine engine;

    auto compressed = engine.compress({.inputPath = tempDir_ / "r1.fastq",
                                       .matePath = tempDir_ / "r2.fastq",
                                       .outputPath = tempDir_ / "paired.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 64 * 1024 * 1024,
                                       .targetFrameBytes = 1,
                                       .forceOverwrite = true});
    ASSERT_TRUE(compressed);
    EXPECT_TRUE(compressed->paired);
    EXPECT_EQ(compressed->recordCount, 2U);

    auto decompressed = engine.decompress({.inputPath = tempDir_ / "paired.fqc",
                                           .outputPath = tempDir_ / "paired.fastq",
                                           .memoryLimitBytes = 64 * 1024 * 1024,
                                           .forceOverwrite = true});
    ASSERT_TRUE(decompressed);
    EXPECT_EQ(readFileContent(tempDir_ / "paired.fastq"),
              "@pair/1\nACGT\n+\nIIII\n@pair/2\nTGCA\n+\nJJJJ\n");
}

TEST_F(ArchiveEngineTest, DetectsAllProfilesAndRejectsAmbiguousLongReads) {
    std::vector<ReadRecord> shortReads = {
        {"read", "", "ACGT", "IIII"},
    };
    auto shortProfile = detectProfile(shortReads);
    ASSERT_TRUE(shortProfile);
    EXPECT_EQ(*shortProfile, format::DatasetProfile::kIllumina);

    std::vector<ReadRecord> ontReads = {
        {"abc", "runid=123 ch=7", std::string(2'000, 'A'), std::string(2'000, 'I')},
    };
    ASSERT_EQ(detectProfile(ontReads).value(), format::DatasetProfile::kOnt);

    std::vector<ReadRecord> hifiReads = {
        {"m64011_220101_010101/42/ccs", "", std::string(2'000, 'A'), std::string(2'000, 'I')},
    };
    ASSERT_EQ(detectProfile(hifiReads).value(), format::DatasetProfile::kPacBioHiFi);

    std::vector<ReadRecord> clrReads = {
        {"m64011_220101_010101/42/0_2000", "", std::string(2'000, 'A'), std::string(2'000, 'I')},
    };
    ASSERT_EQ(detectProfile(clrReads).value(), format::DatasetProfile::kPacBioClr);

    std::vector<ReadRecord> ambiguous = {
        {"unknown", "", std::string(2'000, 'A'), std::string(2'000, 'I')},
    };
    auto ambiguousProfile = detectProfile(ambiguous);
    ASSERT_FALSE(ambiguousProfile);
    EXPECT_EQ(ambiguousProfile.error().code, ErrorCode::kUsageError);
}

TEST_F(ArchiveEngineTest, DetectsEnaRewrittenLongReadsAsOnt) {
    std::vector<ReadRecord> enaOnt = {
        {"DRR171398.1", "1/1", std::string(16'340, 'A'), std::string(16'340, 'I')},
    };
    auto ont = detectProfile(enaOnt);
    ASSERT_TRUE(ont) << ont.error().message;
    EXPECT_EQ(*ont, format::DatasetProfile::kOnt);

    std::vector<ReadRecord> enaShort = {
        {"SRR2962693.1", "1/1", std::string(126, 'A'), std::string(126, 'I')},
    };
    ASSERT_EQ(detectProfile(enaShort).value(), format::DatasetProfile::kIllumina);
}

TEST_F(ArchiveEngineTest, RejectsPairedCountMismatchAndTinyMemoryLimit) {
    writeFile(tempDir_ / "r1.fastq", "@a/1\nACGT\n+\nIIII\n@b/1\nACGT\n+\nIIII\n");
    writeFile(tempDir_ / "r2.fastq", "@a/2\nTGCA\n+\nJJJJ\n");
    ArchiveEngine engine;

    auto mismatch = engine.compress({.inputPath = tempDir_ / "r1.fastq",
                                     .matePath = tempDir_ / "r2.fastq",
                                     .outputPath = tempDir_ / "bad.fqc",
                                     .profile = format::DatasetProfile::kIllumina,
                                     .memoryLimitBytes = 64 * 1024 * 1024,
                                     .forceOverwrite = true});
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code, ErrorCode::kFormatError);

    auto tinyMemory = engine.compress({.inputPath = tempDir_ / "r1.fastq",
                                       .matePath = {},
                                       .outputPath = tempDir_ / "tiny.fqc",
                                       .profile = format::DatasetProfile::kIllumina,
                                       .memoryLimitBytes = 1024,
                                       .forceOverwrite = true});
    ASSERT_FALSE(tinyMemory);
    EXPECT_EQ(tinyMemory.error().code, ErrorCode::kUsageError);
}

}  // namespace fqc::commands::test
