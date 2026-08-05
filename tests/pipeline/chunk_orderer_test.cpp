// =============================================================================
// fq-compressor - Chunk Orderer Unit Tests
// =============================================================================

#include "fqc/pipeline/chunk_orderer.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using fqc::pipeline::ChunkOrderer;

TEST(ChunkOrdererTest, InOrderPassesThrough) {
    ChunkOrderer<int> orderer;
    EXPECT_TRUE(orderer.submitFrame(0, 0, 10).size() == 1U);
    auto ready = orderer.submitFrame(0, 1, 11);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], 11);
}

TEST(ChunkOrdererTest, OutOfOrderFramesHeldUntilGapFills) {
    ChunkOrderer<int> orderer;
    EXPECT_TRUE(orderer.submitFrame(0, 1, 11).empty());  // (0,0) missing
    EXPECT_TRUE(orderer.submitFrame(0, 2, 12).empty());
    auto ready = orderer.submitFrame(0, 0, 10);
    ASSERT_EQ(ready.size(), 3U);
    EXPECT_EQ(ready[0], 10);
    EXPECT_EQ(ready[1], 11);
    EXPECT_EQ(ready[2], 12);
}

TEST(ChunkOrdererTest, ChunkAdvancesOnlyAfterMarkerAndAllFrames) {
    ChunkOrderer<int> orderer;
    // Chunk 0 produces 2 frames; chunk 1's frame arrives first and is held.
    EXPECT_TRUE(orderer.submitFrame(1, 0, 100).empty());
    // (0,0) is at the cursor and drains immediately.
    auto first = orderer.submitFrame(0, 0, 10);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first[0], 10);
    // Marker overtakes the remaining frame: chunk must NOT advance yet
    // ((0,1) still missing even though the marker says total=2).
    EXPECT_TRUE(orderer.submitChunkEnd(0, 2).empty());
    auto ready = orderer.submitFrame(0, 1, 11);
    // Draining (0,1) completes chunk 0 -> (1,0) unblocks.
    ASSERT_EQ(ready.size(), 2U);
    EXPECT_EQ(ready[0], 11);
    EXPECT_EQ(ready[1], 100);
}

TEST(ChunkOrdererTest, ZeroFrameChunkThenFrameDrainsInOrder) {
    ChunkOrderer<int> orderer;
    orderer.submitChunkEnd(0, 0);                // chunk 0 done, cursor -> chunk 1
    auto ready = orderer.submitFrame(1, 0, 42);  // cursor at (1,0): immediate
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], 42);
}

TEST(ChunkOrdererTest, MarkerForFutureChunkDoesNotPrematurelyAdvance) {
    ChunkOrderer<int> orderer;
    orderer.submitChunkEnd(1, 0);  // chunk 1 (empty) marker arrives before chunk 0
    auto drained = orderer.submitFrame(0, 0, 7);
    ASSERT_EQ(drained.size(), 1U);  // (0,0) drains immediately
    EXPECT_EQ(drained[0], 7);
    // chunk 0's marker completes it; cursor then advances over empty chunk 1.
    EXPECT_TRUE(orderer.submitChunkEnd(0, 1).empty());
    // Chunk 2 must now be the cursor: its frame drains immediately.
    auto ready = orderer.submitFrame(2, 0, 9);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], 9);
}
