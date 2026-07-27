// =============================================================================
// fq-compressor - Compression Pipeline Integration Tests
// =============================================================================

#include "fqc/common/error.h"
#include "fqc/format/archive.h"
#include "fqc/pipeline/compress_pipeline.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using fqc::ErrorCode;
using fqc::ReadRecord;
using fqc::format::ArchiveWriter;
using fqc::format::DatasetProfile;
using fqc::pipeline::CompressPipeline;

namespace {

[[nodiscard]] auto makeFastq(int count) -> std::string {
    std::string fastq;
    for (int i = 0; i < count; ++i) {
        fastq += "@read" + std::to_string(i) + " comment\n";
        fastq += "ACGTACGT\n";
        fastq += "+\n";
        fastq += "IIIIIIII\n";
    }
    return fastq;
}

[[nodiscard]] auto makeRecord(std::string id) -> ReadRecord {
    return {std::move(id), "", "ACGTACGT", "IIIIIIII"};
}

}  // namespace

TEST(CompressPipelineTest, BasicRun) {
    constexpr int kRecordCount = 8;
    std::istringstream input(makeFastq(kRecordCount));
    std::ostringstream output;

    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    CompressPipeline pipeline(4096);

    auto result = pipeline.run(input, nullptr, {}, writer);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->recordCount, kRecordCount);
    EXPECT_GE(result->frameCount, 1U);
    EXPECT_FALSE(output.str().empty());
}

TEST(CompressPipelineTest, EmptyInput) {
    std::istringstream input;
    std::ostringstream output;

    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    CompressPipeline pipeline(4096);

    auto result = pipeline.run(input, nullptr, {}, writer);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->recordCount, 0U);
    EXPECT_EQ(result->frameCount, 0U);
}

TEST(CompressPipelineTest, MultipleFrames) {
    constexpr int kRecordCount = 100;
    std::istringstream input(makeFastq(kRecordCount));
    std::ostringstream output;

    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    CompressPipeline pipeline(256);

    auto result = pipeline.run(input, nullptr, {}, writer);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->recordCount, kRecordCount);
    EXPECT_GT(result->frameCount, 1U);
}

TEST(CompressPipelineTest, InitialRecordsEmittedFirst) {
    std::vector<ReadRecord> initial{makeRecord("s0"), makeRecord("s1")};
    std::istringstream input(makeFastq(3));
    std::ostringstream output;

    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    CompressPipeline pipeline(4096);

    auto result = pipeline.run(input, nullptr, initial, writer);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->recordCount, 5U);  // 2 initial + 3 streamed
}

TEST(CompressPipelineTest, PairedRun) {
    std::istringstream primary(makeFastq(5));
    std::istringstream mate(makeFastq(5));
    std::ostringstream output;

    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina, .paired = true});
    CompressPipeline pipeline(4096, true);

    auto result = pipeline.run(primary, &mate, {}, writer);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->recordCount, 10U);
}

TEST(CompressPipelineTest, WriterFailureAbortsReaderWithoutDeadlock) {
    // Enough records to fill the queue depth several times over, so the reader
    // is blocked on a full push when the writer fails. The writer must abort
    // the queue so the reader unblocks; otherwise run() would hang forever on
    // reader.join().
    std::istringstream input(makeFastq(5000));
    std::ostringstream output;
    output.setstate(std::ios::failbit);  // force writeFrame to fail immediately

    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    CompressPipeline pipeline(256);

    auto result = pipeline.run(input, nullptr, {}, writer);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kIOError);
}

TEST(CompressPipelineTest, EncoderFailureAbortsPipeline) {
    // maxFrameBytes=1 makes every frame's raw streams exceed the limit, so
    // encodeFrame returns kUsageError. The first encoder to hit it records the
    // error and request_stop; the pipeline must unwind without deadlock.
    std::istringstream input(makeFastq(50));
    std::ostringstream output;
    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina, .maxFrameBytes = 1});
    CompressPipeline pipeline(256);

    auto result = pipeline.run(input, nullptr, {}, writer);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kUsageError);
}

TEST(CompressPipelineTest, SingleEncoderProducesValidArchive) {
    // parallelism=1 degenerates to a single encoder + reorder buffer (always
    // in-order submission). Verifies the N=1 path still produces a correct
    // archive with the right counts.
    constexpr int kRecordCount = 100;
    std::istringstream input(makeFastq(kRecordCount));
    std::ostringstream output;

    ArchiveWriter writer(output, {.profile = DatasetProfile::kIllumina});
    CompressPipeline pipeline(256, false, 1);

    auto result = pipeline.run(input, nullptr, {}, writer);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->recordCount, kRecordCount);
    EXPECT_GT(result->frameCount, 1U);
    EXPECT_FALSE(output.str().empty());
}

TEST(CompressPipelineTest, ParallelEncodingIsDeterministic) {
    // Same input run twice with N=4 must produce byte-identical output:
    // frameId is assigned by the single-threaded reader (deterministic), and
    // the reorder buffer restores in-order submission regardless of how the N
    // encoders finish. Catches regressions where parallel completion leaks
    // into output order.
    const std::string input = makeFastq(500);

    std::ostringstream out1;
    {
        std::istringstream in(input);
        ArchiveWriter writer(out1, {.profile = DatasetProfile::kIllumina});
        CompressPipeline pipeline(128, false, 4);
        ASSERT_TRUE(pipeline.run(in, nullptr, {}, writer).has_value());
    }

    std::ostringstream out2;
    {
        std::istringstream in(input);
        ArchiveWriter writer(out2, {.profile = DatasetProfile::kIllumina});
        CompressPipeline pipeline(128, false, 4);
        ASSERT_TRUE(pipeline.run(in, nullptr, {}, writer).has_value());
    }

    EXPECT_EQ(out1.str(), out2.str());
}
