// =============================================================================
// fq-compressor - Reorder Buffer Tests
// =============================================================================

#include "fqc/pipeline/reorder_buffer.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

using fqc::pipeline::ReorderBuffer;

TEST(ReorderBufferTest, InOrderSubmitReturnsImmediately) {
    ReorderBuffer<int> rb;
    auto r0 = rb.submit(0, 100);
    ASSERT_EQ(r0.size(), 1u);
    EXPECT_EQ(r0[0], 100);
    auto r1 = rb.submit(1, 101);
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_EQ(r1[0], 101);
    EXPECT_EQ(rb.pendingCount(), 0u);
    EXPECT_EQ(rb.nextId(), 2u);
}

TEST(ReorderBufferTest, OutOfOrderBuffersUntilGapFilled) {
    ReorderBuffer<int> rb;

    // id 2 arrives first -- gap at 0,1, buffer it.
    auto r2 = rb.submit(2, 200);
    EXPECT_TRUE(r2.empty());
    EXPECT_EQ(rb.pendingCount(), 1u);

    // id 0 arrives -- drain 0, but 1 still missing so 2 stays buffered.
    auto r0 = rb.submit(0, 0);
    ASSERT_EQ(r0.size(), 1u);
    EXPECT_EQ(r0[0], 0);
    EXPECT_EQ(rb.pendingCount(), 1u);

    // id 1 fills the gap -- drain 1, then 2.
    auto r1 = rb.submit(1, 1);
    ASSERT_EQ(r1.size(), 2u);
    EXPECT_EQ(r1[0], 1);
    EXPECT_EQ(r1[1], 200);
    EXPECT_EQ(rb.pendingCount(), 0u);
    EXPECT_EQ(rb.nextId(), 3u);
}

TEST(ReorderBufferTest, MultipleContiguousDrain) {
    // Completion order: 3, 1, 0, 2. After 0 we drain {0,1}; after 2 we drain
    // {2,3}. Exercises both a partial drain and a multi-element drain.
    ReorderBuffer<int> rb;

    EXPECT_TRUE(rb.submit(3, 300).empty());  // pending {3}
    EXPECT_TRUE(rb.submit(1, 100).empty());  // pending {1,3}

    auto r0 = rb.submit(0, 0);  // drain 0,1 -> pending {3}
    ASSERT_EQ(r0.size(), 2u);
    EXPECT_EQ(r0[0], 0);
    EXPECT_EQ(r0[1], 100);
    EXPECT_EQ(rb.pendingCount(), 1u);

    auto r2 = rb.submit(2, 200);  // drain 2,3
    ASSERT_EQ(r2.size(), 2u);
    EXPECT_EQ(r2[0], 200);
    EXPECT_EQ(r2[1], 300);
    EXPECT_EQ(rb.pendingCount(), 0u);
    EXPECT_EQ(rb.nextId(), 4u);
}

TEST(ReorderBufferTest, DuplicateCommittedIdDropped) {
    ReorderBuffer<int> rb;
    auto r0 = rb.submit(0, 0);
    ASSERT_EQ(r0.size(), 1u);

    auto dup = rb.submit(0, 999);  // 0 already committed
    EXPECT_TRUE(dup.empty());
    EXPECT_EQ(rb.nextId(), 1u);
}

TEST(ReorderBufferTest, UniquePtrMoveSemantics) {
    // The pipeline carries unique_ptr<EncodedFrame> through the reorder
    // buffer. Verify move-out from pending_ is correct -- the buffered frame
    // at id 2 must survive the gap and arrive intact once the gap fills.
    ReorderBuffer<std::unique_ptr<int>> rb;

    EXPECT_TRUE(rb.submit(2, std::make_unique<int>(200)).empty());
    auto r0 = rb.submit(0, std::make_unique<int>(0));
    ASSERT_EQ(r0.size(), 1u);
    EXPECT_EQ(*r0[0], 0);

    auto r1 = rb.submit(1, std::make_unique<int>(100));
    ASSERT_EQ(r1.size(), 2u);
    EXPECT_EQ(*r1[0], 100);
    EXPECT_EQ(*r1[1], 200);  // the previously-buffered frame, moved out intact
    EXPECT_EQ(rb.pendingCount(), 0u);
}
