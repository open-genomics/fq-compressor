// =============================================================================
// fq-compressor - SPSC Queue Tests
// =============================================================================

#include "fqc/pipeline/spsc_queue.h"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using fqc::pipeline::SpscQueue;

TEST(SpscQueueTest, PushPopSingleItem) {
    SpscQueue<int, 4> queue;
    EXPECT_TRUE(queue.push(42));
    auto item = queue.pop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, 42);
}

TEST(SpscQueueTest, FillToCapacityAndDrain) {
    SpscQueue<int, 4> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);

    EXPECT_EQ(*queue.pop(), 1);
    EXPECT_EQ(*queue.pop(), 2);
    EXPECT_EQ(*queue.pop(), 3);
}

TEST(SpscQueueTest, BackpressureUnblocksOnPop) {
    SpscQueue<int, 2> queue;
    queue.push(1);

    bool pushed = false;
    std::thread producer([&] {
        EXPECT_TRUE(queue.push(2));
        pushed = true;
    });

    EXPECT_EQ(*queue.pop(), 1);
    producer.join();
    EXPECT_TRUE(pushed);
    EXPECT_EQ(*queue.pop(), 2);
}

TEST(SpscQueueTest, CloseReturnsNulloptAfterDrain) {
    SpscQueue<int, 4> queue;
    queue.push(10);
    queue.push(20);
    queue.close();

    EXPECT_EQ(*queue.pop(), 10);
    EXPECT_EQ(*queue.pop(), 20);
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(SpscQueueTest, CloseOnEmptyReturnsNullopt) {
    SpscQueue<int, 4> queue;
    queue.close();
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(SpscQueueTest, AbortUnblocksFullPush) {
    // capacity 2 holds one item (ring leaves one slot empty to separate
    // full from empty). After one push the next push would block; abort
    // must make it return false instead of hanging.
    SpscQueue<int, 2> queue;
    EXPECT_TRUE(queue.push(1));
    queue.abort();
    EXPECT_FALSE(queue.push(2));
    EXPECT_TRUE(queue.isAborted());
}

TEST(SpscQueueTest, AbortReturnsNulloptFromEmptyPop) {
    SpscQueue<int, 4> queue;
    queue.abort();
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(SpscQueueTest, StressProducerConsumer) {
    constexpr std::size_t kItemCount = 10000;
    SpscQueue<std::size_t, 8> queue;

    std::thread producer([&] {
        for (std::size_t i = 0; i < kItemCount; ++i) {
            queue.push(i);
        }
        queue.close();
    });

    std::vector<std::size_t> received;
    received.reserve(kItemCount);
    std::thread consumer([&] {
        while (true) {
            auto item = queue.pop();
            if (!item.has_value()) {
                break;
            }
            received.push_back(*item);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kItemCount);
    for (std::size_t i = 0; i < kItemCount; ++i) {
        EXPECT_EQ(received[i], i);
    }
}

TEST(SpscQueueTest, StopTokenUnblocksFullPush) {
    // capacity 2 holds one item; push(2) would block. request_stop cancels it
    // through the stop_token CV wait and push returns false.
    SpscQueue<int, 2> queue;
    std::stop_source ss;
    auto token = ss.get_token();
    EXPECT_TRUE(queue.push(1, token));
    bool pushed = false;
    std::thread producer([&] {
        EXPECT_FALSE(queue.push(2, token));
        pushed = true;
    });
    ss.request_stop();
    producer.join();
    EXPECT_TRUE(pushed);
}

TEST(SpscQueueTest, StopTokenReturnsNulloptFromEmptyPop) {
    SpscQueue<int, 4> queue;
    std::stop_source ss;
    auto token = ss.get_token();
    bool popped = false;
    std::thread consumer([&] {
        EXPECT_FALSE(queue.pop(token).has_value());
        popped = true;
    });
    ss.request_stop();
    consumer.join();
    EXPECT_TRUE(popped);
}
