// =============================================================================
// fq-compressor - Decompression Pipeline Integration Tests
// =============================================================================

#include "fqc/pipeline/decompress_pipeline.h"

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/format/archive.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using fqc::ErrorCode;
using fqc::ReadRecord;
using fqc::VoidResult;
using fqc::format::ArchiveWriter;
using fqc::format::DatasetProfile;
using fqc::pipeline::DecompressPipeline;

namespace {

[[nodiscard]] auto makeRecords(int count, int sequenceLength = 150) -> std::vector<ReadRecord> {
    std::vector<ReadRecord> records;
    records.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        records.push_back({"read_" + std::to_string(i),
                           "comment_" + std::to_string(i),
                           std::string(static_cast<std::size_t>(sequenceLength), 'A'),
                           std::string(static_cast<std::size_t>(sequenceLength), 'I')});
    }
    return records;
}

// Write a multi-frame archive (small maxFrameBytes forces many frames).
[[nodiscard]] auto makeArchive(const std::vector<ReadRecord>& records,
                               std::size_t recordsPerFrame) -> std::string {
    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output,
                         {.profile = DatasetProfile::kIllumina, .maxFrameBytes = 1024 * 1024});
    for (std::size_t offset = 0; offset < records.size(); offset += recordsPerFrame) {
        const auto count = std::min(recordsPerFrame, records.size() - offset);
        EXPECT_TRUE(writer.writeFrame(std::span(records).subspan(offset, count)));
    }
    EXPECT_TRUE(writer.finish());
    return output.str();
}

[[nodiscard]] auto collectSink(std::vector<ReadRecord>& sink) -> DecompressPipeline::RecordSink {
    return [&sink](std::vector<ReadRecord> records) -> VoidResult {
        sink.insert(sink.end(),
                    std::make_move_iterator(records.begin()),
                    std::make_move_iterator(records.end()));
        return {};
    };
}

}  // namespace

TEST(DecompressPipelineTest, BasicRunPreservesOrderAndCounts) {
    const auto records = makeRecords(97);
    const auto archive = makeArchive(records, 8);
    std::istringstream input(archive, std::ios::binary);

    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 4);
    std::vector<ReadRecord> collected;
    auto result = pipeline.run(input, collectSink(collected));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(collected, records);
    EXPECT_GT(result->frameCount, 1U);
    EXPECT_EQ(result->recordCount, records.size());
    EXPECT_EQ(result->totalBases, records.size() * 150U);
    EXPECT_EQ(result->encodedBytes, archive.size());
    EXPECT_EQ(result->metadata.profile, DatasetProfile::kIllumina);
}

TEST(DecompressPipelineTest, SingleDecoderPreservesOrder) {
    const auto records = makeRecords(50);
    const auto archive = makeArchive(records, 4);
    std::istringstream input(archive, std::ios::binary);

    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 1);
    std::vector<ReadRecord> collected;
    auto result = pipeline.run(input, collectSink(collected));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(collected, records);
}

TEST(DecompressPipelineTest, EmptyArchiveSucceeds) {
    std::ostringstream output(std::ios::binary);
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    ASSERT_TRUE(writer.finish());

    std::istringstream input(output.str(), std::ios::binary);
    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 4);
    std::vector<ReadRecord> collected;
    auto result = pipeline.run(input, collectSink(collected));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(collected.empty());
    EXPECT_EQ(result->frameCount, 0U);
    EXPECT_EQ(result->recordCount, 0U);
}

TEST(DecompressPipelineTest, TamperedFooterGlobalChecksumFails) {
    auto archive = makeArchive(makeRecords(20), 5);
    ASSERT_GT(archive.size(), 8U);
    // The rolling global checksum is the last 8 bytes of the 40-byte footer.
    archive[archive.size() - 1] ^= 0xFFU;

    std::istringstream input(archive, std::ios::binary);
    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 4);
    std::vector<ReadRecord> collected;
    auto result = pipeline.run(input, collectSink(collected));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kChecksumError);
}

TEST(DecompressPipelineTest, TamperedFooterTotalsFail) {
    auto archive = makeArchive(makeRecords(20), 5);
    ASSERT_GT(archive.size(), 24U);
    // Footer layout: magic(4) size(4) frameCount(8) recordCount(8) ...
    // recordCount starts at footer+16 == size-24.
    archive[archive.size() - 24] ^= 0x01U;

    std::istringstream input(archive, std::ios::binary);
    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 4);
    std::vector<ReadRecord> collected;
    auto result = pipeline.run(input, collectSink(collected));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
}

TEST(DecompressPipelineTest, CorruptedPayloadCannotPassSilently) {
    auto archive = makeArchive(makeRecords(20), 5);
    ASSERT_GT(archive.size(), 41U);
    // Flip the last payload byte (right before the 40-byte footer). zstd may
    // reject the stream outright (kFormatError), or decompress to wrong bytes
    // which the per-frame logical checksum then catches (kChecksumError) --
    // either way the corruption must surface, never pass silently.
    archive[archive.size() - 41] ^= 0xFFU;

    std::istringstream input(archive, std::ios::binary);
    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 4);
    std::vector<ReadRecord> collected;
    auto result = pipeline.run(input, collectSink(collected));

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code == ErrorCode::kChecksumError ||
                result.error().code == ErrorCode::kFormatError)
        << "unexpected error code: " << static_cast<int>(result.error().code);
}

TEST(DecompressPipelineTest, TruncatedArchiveFails) {
    auto archive = makeArchive(makeRecords(20), 5);
    archive.resize(archive.size() / 2);

    std::istringstream input(archive, std::ios::binary);
    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 4);
    std::vector<ReadRecord> collected;
    auto result = pipeline.run(input, collectSink(collected));

    ASSERT_FALSE(result.has_value());
}

TEST(DecompressPipelineTest, SinkFailureCancelsPipelineWithoutDeadlock) {
    const auto records = makeRecords(500);
    const auto archive = makeArchive(records, 10);
    std::istringstream input(archive, std::ios::binary);

    DecompressPipeline pipeline(1024 * 1024, fqc::format::kDefaultMemoryLimitBytes, 4);
    std::size_t calls = 0;
    auto result = pipeline.run(input, [&calls](std::vector<ReadRecord>) -> VoidResult {
        ++calls;
        if (calls >= 2) {
            return fqc::makeVoidError(ErrorCode::kIOError, "simulated sink failure");
        }
        return {};
    });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kIOError);
}
