// =============================================================================
// fq-compressor - Frame Accumulator Unit Tests
// =============================================================================

#include "fqc/pipeline/frame_accumulator.h"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using fqc::ReadRecord;
using fqc::pipeline::FrameAccumulator;

namespace {

[[nodiscard]] auto makeRecord(std::string id, std::size_t sequenceLength = 8) -> ReadRecord {
    return {std::move(id), "", std::string(sequenceLength, 'A'), std::string(sequenceLength, 'I')};
}

[[nodiscard]] auto retainedOf(const ReadRecord& record) -> std::size_t {
    return FrameAccumulator::retainedBytes(record);
}

}  // namespace

TEST(FrameAccumulatorTest, ClosesExactlyAtRetainedBudget) {
    // Budget is exactly three records' worth of the same records being
    // appended (same id size => same retained cost).
    const auto perRecord = retainedOf(makeRecord("r1"));
    FrameAccumulator accumulator(perRecord * 3);

    EXPECT_FALSE(accumulator.append(makeRecord("r1")).has_value());
    EXPECT_FALSE(accumulator.append(makeRecord("r2")).has_value());
    auto closed = accumulator.append(makeRecord("r3"));
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->size(), 3U);
    EXPECT_FALSE(accumulator.finish().has_value());  // nothing pending after close
}

TEST(FrameAccumulatorTest, FinishFlushesRemainderOnce) {
    FrameAccumulator accumulator(1ULL << 30);
    accumulator.append(makeRecord("r1"));
    accumulator.append(makeRecord("r2"));

    auto tail = accumulator.finish();
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->size(), 2U);
    EXPECT_FALSE(accumulator.finish().has_value());
}

TEST(FrameAccumulatorTest, PairedClosesOnlyOnEvenRecordCount) {
    const auto perRecord = retainedOf(makeRecord("probe"));
    FrameAccumulator accumulator(perRecord * 3, /*paired=*/true);

    // Budget reached after 3 records, but paired mode must wait for an even
    // count so a pair is never split across frames.
    EXPECT_FALSE(accumulator.append(makeRecord("r1")).has_value());
    EXPECT_FALSE(accumulator.append(makeRecord("r2")).has_value());
    EXPECT_FALSE(accumulator.append(makeRecord("r3")).has_value());
    auto closed = accumulator.append(makeRecord("r4"));
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->size(), 4U);
}

TEST(FrameAccumulatorTest, RetainedBytesIsContentDeterministic) {
    // Framing accounting must be a pure function of content (sizes), never
    // of string capacities -- capacities depend on the stream buffering
    // phase and would make frame boundaries non-deterministic across paths.
    ReadRecord record{"id", "comment", std::string(150, 'A'), std::string(150, 'I')};
    const auto expected = sizeof(ReadRecord) + record.id.size() + 1 + record.comment.size() + 1 +
        record.sequence.size() + 1 + record.quality.size() + 1;
    EXPECT_EQ(FrameAccumulator::retainedBytes(record), expected);

    // Same content, artificially inflated capacity: the accounting must not
    // change (the pre-fix capacity-based rule would have counted the slack).
    ReadRecord inflated = record;
    inflated.sequence.reserve(4096);
    EXPECT_EQ(FrameAccumulator::retainedBytes(inflated), FrameAccumulator::retainedBytes(record));
}
