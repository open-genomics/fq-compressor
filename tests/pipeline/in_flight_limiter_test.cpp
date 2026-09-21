// =============================================================================
// fq-compressor - In-Flight Limiter Tests
// =============================================================================

#include "fqc/pipeline/in_flight_limiter.h"

#include <atomic>
#include <future>
#include <thread>

#include <gtest/gtest.h>

using fqc::pipeline::InFlightLimiter;

TEST(InFlightLimiterTest, BlocksAboveCapacityUntilCreditIsReleased) {
    InFlightLimiter limiter(1);
    ASSERT_TRUE(limiter.acquire());

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::atomic<bool> acquired{false};
    std::jthread waiter([&](std::stop_token st) {
        entered.set_value();
        acquired.store(limiter.acquire(st));
    });

    enteredFuture.wait();
    EXPECT_FALSE(acquired.load());
    limiter.release();
    waiter.join();

    EXPECT_TRUE(acquired.load());
    EXPECT_EQ(limiter.highWater(), 1U);
    limiter.release();
}

TEST(InFlightLimiterTest, StopTokenCancelsBlockedAcquire) {
    InFlightLimiter limiter(1);
    ASSERT_TRUE(limiter.acquire());

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::stop_source stopSource;
    std::atomic<bool> acquired{true};
    std::jthread waiter([&](std::stop_token) {
        entered.set_value();
        acquired.store(limiter.acquire(stopSource.get_token()));
    });

    enteredFuture.wait();
    stopSource.request_stop();
    waiter.join();

    EXPECT_FALSE(acquired.load());
    limiter.release();
}

TEST(InFlightLimiterTest, OwnerReservationPreventsCrossOwnerStarvation) {
    InFlightLimiter limiter(2, 2);
    ASSERT_TRUE(limiter.acquire(0));
    ASSERT_TRUE(limiter.acquire(1));

    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::atomic<bool> acquired{false};
    std::jthread waiter([&](std::stop_token st) {
        entered.set_value();
        acquired.store(limiter.acquire(0, st));
    });

    enteredFuture.wait();
    EXPECT_FALSE(acquired.load());
    limiter.release(0);
    waiter.join();

    EXPECT_TRUE(acquired.load());
    limiter.release(0);
    limiter.release(1);
}
