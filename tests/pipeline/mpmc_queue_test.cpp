// =============================================================================
// fq-compressor - MPMC Queue Tests
// =============================================================================

#include "fqc/pipeline/mpmc_queue.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using fqc::pipeline::MpmcQueue;

TEST(MpmcQueueTest, PushPopSingleItem) {
    MpmcQueue<int, 4> queue;
    EXPECT_TRUE(queue.push(42));
    auto item = queue.pop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, 42);
}

TEST(MpmcQueueTest, FillToCapacityAndDrain) {
    MpmcQueue<int, 4> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);

    EXPECT_EQ(*queue.pop(), 1);
    EXPECT_EQ(*queue.pop(), 2);
    EXPECT_EQ(*queue.pop(), 3);
}

TEST(MpmcQueueTest, BackpressureUnblocksOnPop) {
    MpmcQueue<int, 2> queue;
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

TEST(MpmcQueueTest, CloseReturnsNulloptAfterDrain) {
    MpmcQueue<int, 4> queue;
    queue.push(10);
    queue.push(20);
    queue.close();

    EXPECT_EQ(*queue.pop(), 10);
    EXPECT_EQ(*queue.pop(), 20);
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(MpmcQueueTest, StopTokenUnblocksFullPush) {
    MpmcQueue<int, 2> queue;
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

TEST(MpmcQueueTest, StopTokenReturnsNulloptFromEmptyPop) {
    MpmcQueue<int, 4> queue;
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

// N producers, one consumer: no item lost, no id corrupted.
TEST(MpmcQueueTest, MultiProducerSingleConsumerNoLoss) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 2000;
    constexpr std::size_t kTotal = kProducers * kPerProducer;
    MpmcQueue<std::size_t, 8> queue;

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                queue.push(p * kPerProducer + i);
            }
        });
    }

    std::vector<unsigned> seen(kTotal, 0);
    std::thread consumer([&] {
        while (true) {
            auto item = queue.pop();
            if (!item.has_value()) {
                break;
            }
            ASSERT_LT(*item, kTotal);
            ++seen[*item];
        }
    });

    for (auto& t : producers)
        t.join();
    queue.close();
    consumer.join();

    for (std::size_t i = 0; i < kTotal; ++i) {
        EXPECT_EQ(seen[i], 1u) << "value " << i << " seen " << seen[i] << " times";
    }
}

// One producer, N consumers: every item delivered to exactly one consumer.
TEST(MpmcQueueTest, SingleProducerMultiConsumerNoDuplicate) {
    constexpr std::size_t kConsumers = 4;
    constexpr std::size_t kTotal = 8000;
    MpmcQueue<std::size_t, 8> queue;

    std::vector<std::vector<std::size_t>> consumed(kConsumers);
    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (std::size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&, c] {
            while (true) {
                auto item = queue.pop();
                if (!item.has_value())
                    break;
                consumed[c].push_back(*item);
            }
        });
    }

    std::thread producer([&] {
        for (std::size_t i = 0; i < kTotal; ++i)
            queue.push(i);
        queue.close();
    });

    producer.join();
    for (auto& t : consumers)
        t.join();

    std::vector<std::size_t> all;
    all.reserve(kTotal);
    for (auto& v : consumed)
        all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    ASSERT_EQ(all.size(), kTotal);
    for (std::size_t i = 0; i < kTotal; ++i) {
        EXPECT_EQ(all[i], i) << "missing/duplicate at " << i;
    }
}

// N producers, M consumers: the full MPMC stress test -- no loss, no
// duplication, no corruption under concurrent fan-in and fan-out.
TEST(MpmcQueueTest, StressMultiProducerMultiConsumer) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    constexpr std::size_t kPerProducer = 2500;
    constexpr std::size_t kTotal = kProducers * kPerProducer;
    MpmcQueue<std::size_t, 16> queue;

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                queue.push(p * kPerProducer + i);
            }
        });
    }

    std::vector<std::vector<std::size_t>> consumed(kConsumers);
    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (std::size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&, c] {
            while (true) {
                auto item = queue.pop();
                if (!item.has_value())
                    break;
                consumed[c].push_back(*item);
            }
        });
    }

    for (auto& t : producers)
        t.join();
    queue.close();
    for (auto& t : consumers)
        t.join();

    std::vector<std::size_t> all;
    all.reserve(kTotal);
    for (auto& v : consumed)
        all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());

    ASSERT_EQ(all.size(), kTotal);
    for (std::size_t i = 0; i < kTotal; ++i) {
        EXPECT_EQ(all[i], i) << "missing/duplicate at " << i;
    }
}

// Multiple consumers blocked on pop must all wake when close() is called --
// otherwise consumers.clear() would hang on join. Exercises the notify_all
// path in close() with several waiters.
TEST(MpmcQueueTest, CloseWakesAllBlockedConsumers) {
    MpmcQueue<int, 4> queue;
    queue.push(42);
    constexpr int kConsumers = 3;
    std::atomic<int> returned{0};
    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            while (queue.pop().has_value())
                ++returned;
        });
    }
    queue.close();
    for (auto& t : consumers)
        t.join();                   // hangs if close didn't wake every blocked consumer
    EXPECT_EQ(returned.load(), 1);  // the one pushed item, consumed exactly once
}

TEST(MpmcQueueTest, StatsCountPushPopAndHighWater) {
    MpmcQueue<int, 8> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);  // occupancy peaks at 3
    EXPECT_EQ(*queue.pop(), 1);
    queue.push(4);  // occupancy back to 3, must not raise the mark

    const auto stats = queue.stats();
    EXPECT_EQ(stats.pushes, 4U);
    EXPECT_EQ(stats.pops, 1U);
    EXPECT_EQ(stats.highWater, 3U);
    EXPECT_EQ(stats.pushWaits, 0U);
    EXPECT_EQ(stats.popWaits, 0U);
}

TEST(MpmcQueueTest, StatsCountBlockedPush) {
    MpmcQueue<int, 2> queue;  // holds Capacity-1 = 1 item
    queue.push(1);

    std::thread producer([&] { EXPECT_TRUE(queue.push(2)); });
    // Poll until the producer is observed blocked on the full queue; bounded
    // so a broken wait-predicate can't hang the test.
    for (int i = 0; i < 100000 && queue.stats().pushWaits == 0; ++i) {
        std::this_thread::yield();
    }
    EXPECT_EQ(queue.stats().pushWaits, 1U);

    EXPECT_EQ(*queue.pop(), 1);
    producer.join();

    const auto stats = queue.stats();
    EXPECT_EQ(stats.pushes, 2U);
    EXPECT_EQ(stats.pops, 1U);
    EXPECT_EQ(stats.highWater, 1U);
}

TEST(MpmcQueueTest, StatsCountBlockedPop) {
    MpmcQueue<int, 4> queue;
    std::optional<int> consumed;
    std::thread consumer([&] { consumed = queue.pop(); });
    for (int i = 0; i < 100000 && queue.stats().popWaits == 0; ++i) {
        std::this_thread::yield();
    }
    EXPECT_EQ(queue.stats().popWaits, 1U);

    queue.push(7);
    consumer.join();
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(*consumed, 7);

    const auto stats = queue.stats();
    EXPECT_EQ(stats.pops, 1U);
    EXPECT_EQ(stats.popWaits, 1U);
}

TEST(MpmcQueueTest, StatsStressPushesEqualPops) {
    constexpr std::size_t kProducers = 2;
    constexpr std::size_t kPerProducer = 1000;
    constexpr std::size_t kTotal = kProducers * kPerProducer;
    MpmcQueue<std::size_t, 8> queue;

    std::vector<std::thread> producers;
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                queue.push(p * kPerProducer + i);
            }
        });
    }
    std::size_t consumed = 0;
    std::thread consumer([&] {
        while (queue.pop().has_value())
            ++consumed;
    });

    for (auto& t : producers)
        t.join();
    queue.close();
    consumer.join();

    ASSERT_EQ(consumed, kTotal);
    const auto stats = queue.stats();
    EXPECT_EQ(stats.pushes, kTotal);
    EXPECT_EQ(stats.pops, kTotal);
    EXPECT_GT(stats.highWater, 0U);
    EXPECT_LE(stats.highWater, 7U);  // Capacity-1 usable slots
}

// A closed queue must refuse new pushes: close() is the end-of-production
// signal, so a producer pushing afterwards is a caller bug that should be
// surfaced as a failed push instead of an item silently accepted into a
// queue no consumer may revisit.
TEST(MpmcQueueTest, RejectsPushAfterClose) {
    MpmcQueue<int, 4> queue;
    queue.close();
    EXPECT_FALSE(queue.push(42));
}
