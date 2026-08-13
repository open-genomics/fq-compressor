// =============================================================================
// Cross-family magic recognition (FQC-FAMILY-001 / recognize-indexed-fqc-family)
// =============================================================================

#include "fqc/commands/archive_engine.h"
#include "fqc/common/error.h"
#include "fqc/format/archive.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace fqc::format::test {
namespace {

[[nodiscard]] auto fixturePath(std::string_view relative) -> std::string {
    return std::string(FQC_FOREIGN_FIXTURE_DIR) + "/" + std::string(relative);
}

[[nodiscard]] auto sequentialFixturePath() -> std::string {
    return std::string(FQC_SEQUENTIAL_FIXTURE_DIR) + "/frozen_se.fqc";
}

void expectIndexedFamilyReject(const fqc::Error& error) {
    EXPECT_EQ(error.code, ErrorCode::kUnsupportedCodec);
    EXPECT_NE(error.message.find("unsupported FQC format family"), std::string::npos);
    EXPECT_NE(error.message.find("fqc-indexed/v2"), std::string::npos);
    EXPECT_NE(error.message.find("open-genomics/fq-compressor-rust"), std::string::npos);
}

}  // namespace

TEST(FormatFamilyTest, AcceptsOwnSequentialFrozenFixture) {
    std::ifstream stream(sequentialFixturePath(), std::ios::binary);
    ASSERT_TRUE(stream) << sequentialFixturePath();
    ArchiveReader reader(stream);
    auto result = reader.open();
    ASSERT_TRUE(result) << result.error().message;
}

TEST(FormatFamilyTest, RejectsIndexedFrozenFixtureAsKnownFamily) {
    std::ifstream stream(fixturePath("frozen.fqc"), std::ios::binary);
    ASSERT_TRUE(stream) << fixturePath("frozen.fqc");
    ArchiveReader reader(stream);
    auto result = reader.open();
    ASSERT_FALSE(result);
    expectIndexedFamilyReject(result.error());
}

TEST(FormatFamilyTest, RejectsUnknownMagic) {
    std::istringstream input(std::string("NOTAFQC!\x00\x00\x00\x00", 12), std::ios::binary);
    ArchiveReader reader(input);
    auto result = reader.open();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
    EXPECT_NE(result.error().message.find("unknown FQC magic"), std::string::npos);
}

TEST(FormatFamilyTest, RejectsTruncatedMagic) {
    std::istringstream input(std::string("FQC", 3), std::ios::binary);
    ArchiveReader reader(input);
    auto result = reader.open();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
    EXPECT_NE(result.error().message.find("truncated"), std::string::npos);
}

TEST(FormatFamilyTest, VerifyAndDecompressRejectIndexedWithoutOutput) {
    const auto indexed = fixturePath("frozen.fqc");
    const auto outDir = std::filesystem::temp_directory_path() / "fqc_family_reject_out.fastq";
    std::error_code ec;
    std::filesystem::remove(outDir, ec);

    commands::ArchiveEngine engine;
    auto verify = engine.verify(indexed, commands::kDefaultMemoryLimitBytes);
    ASSERT_FALSE(verify);
    expectIndexedFamilyReject(verify.error());

    commands::DecompressionRequest request;
    request.inputPath = indexed;
    request.outputPath = outDir;
    request.forceOverwrite = true;
    request.memoryLimitBytes = commands::kDefaultMemoryLimitBytes;
    auto decompress = engine.decompress(request);
    ASSERT_FALSE(decompress);
    expectIndexedFamilyReject(decompress.error());
    EXPECT_FALSE(std::filesystem::exists(outDir));
}

}  // namespace fqc::format::test
