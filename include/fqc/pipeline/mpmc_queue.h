// =============================================================================
// fq-compressor - MPMC Bounded Queue
// =============================================================================

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace fqc::pipeline {

/// Advisory counters snapshot, see `MpmcQueue::stats`.
struct QueueStats {
    std::uint64_t pushes = 0;     // successful push() calls
    std::uint64_t pops = 0;       // successful pop() calls (item dequeued)
    std::uint64_t pushWaits = 0;  // push() calls that had to block (queue full)
    std::uint64_t popWaits = 0;   // pop() calls that had to block (queue empty)
    std::size_t highWater = 0;    // max items held simultaneously
};

/// Multi-producer multi-consumer bounded ring buffer.
///
/// A mutex plus two `condition_variable_any` serializes all producers and
/// consumers, so the queue is safe for concurrent fan-in and fan-out.
///
/// `close()` is normal end-of-production: every blocked consumer wakes
/// (`notify_all`, since there may be several), drains the remaining items,
/// then returns `std::nullopt`. `push`/`pop` also take a `std::stop_token`: on
/// `request_stop()` a blocked `push` returns `false` and a blocked `pop`
/// returns `std::nullopt` *without draining* -- cooperative cancellation
/// through the CV rather than spinning. `condition_variable_any` (not
/// `condition_variable`) is required for the stop_token `wait` overload.
///
/// Work distribution among N consumers is implicit: each `pop` contends on the
/// mutex, so faster consumers simply claim more items. No work-stealing, no
/// lock-free CAS -- the pipeline's frame granularity is coarse enough that
/// mutex contention is not the bottleneck (see ARCHITECTURE.md).
template <typename T, std::size_t Capacity>
requires(Capacity > 0 && (Capacity & (Capacity - 1)) == 0)
class MpmcQueue {
public:
    MpmcQueue() = default;

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;
    MpmcQueue(MpmcQueue&&) = delete;
    MpmcQueue& operator=(MpmcQueue&&) = delete;

    /// Enqueue an item. Returns false if `st` was stop-requested; otherwise
    /// blocks until space is available and returns true. Safe for concurrent
    /// producers.
    auto push(T item, std::stop_token st = {}) -> bool {
        std::unique_lock lock(m_);
        // close() 是生产结束信号：之后继续入队是调用方 bug，返回 false 暴露之，
        // 否则条目会滞留在一个消费者可能已按 closed+empty 退出的队列里。
        if (closed_) {
            return false;
        }
        // Count "had to block" by evaluating the wait predicate once up front;
        // the lock is held continuously, so the predicate can't change before
        // wait() re-checks it.
        if (!st.stop_requested() && isFullLocked()) {
            counters_.pushWaits.fetch_add(1, std::memory_order_relaxed);
        }
        cvNotFull_.wait(lock, st, [&] { return st.stop_requested() || !isFullLocked(); });
        if (st.stop_requested()) {
            return false;
        }
        storage_[head_] = std::move(item);
        head_ = (head_ + 1) & kMask;
        counters_.pushes.fetch_add(1, std::memory_order_relaxed);
        // Every writer holds the mutex, so a plain max-store can't lose
        // updates -- no CAS loop needed for the high-water mark.
        const std::size_t occupancy = (head_ - tail_) & kMask;
        if (occupancy > counters_.highWater.load(std::memory_order_relaxed)) {
            counters_.highWater.store(occupancy, std::memory_order_relaxed);
        }
        lock.unlock();
        // One item added -> wake exactly one waiter. With multiple consumers
        // blocked on pop, notify_one hands the item to whichever wakes first;
        // the rest re-check the predicate and re-sleep. No item is lost.
        cvNotEmpty_.notify_one();
        return true;
    }

    /// Dequeue the next item, or `std::nullopt`. Returns `std::nullopt` if `st`
    /// was stop-requested (cancel, without draining), or once the queue has
    /// drained after `close()`. Safe for concurrent consumers.
    [[nodiscard]] auto pop(std::stop_token st = {}) -> std::optional<T> {
        std::unique_lock lock(m_);
        if (!st.stop_requested() && !closed_ && isEmptyLocked()) {
            counters_.popWaits.fetch_add(1, std::memory_order_relaxed);
        }
        cvNotEmpty_.wait(
            lock, st, [&] { return st.stop_requested() || closed_ || !isEmptyLocked(); });
        if (st.stop_requested() || isEmptyLocked()) {
            return std::nullopt;
        }
        T item = std::move(storage_[tail_]);
        tail_ = (tail_ + 1) & kMask;
        counters_.pops.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        cvNotFull_.notify_one();
        return item;
    }

    /// Signal that production is complete (no more items will be pushed). All
    /// blocked consumers wake, drain the remaining items, then see `nullopt`.
    void close() {
        {
            const std::lock_guard lock(m_);
            closed_ = true;
        }
        cvNotEmpty_.notify_all();
    }

    /// Advisory counters. Call after producers/consumers have joined for a
    /// consistent snapshot; calling mid-flight yields a racy (but still
    /// individually atomic) approximation. Updates are `memory_order_relaxed`:
    /// every bump is performed by the lock holder, and the stats are never
    /// used for synchronization -- the one legitimate relaxed-atomics use
    /// case, statistics that must not fence the hot path.
    [[nodiscard]] auto stats() const -> QueueStats {
        return {
            .pushes = counters_.pushes.load(std::memory_order_relaxed),
            .pops = counters_.pops.load(std::memory_order_relaxed),
            .pushWaits = counters_.pushWaits.load(std::memory_order_relaxed),
            .popWaits = counters_.popWaits.load(std::memory_order_relaxed),
            .highWater = counters_.highWater.load(std::memory_order_relaxed),
        };
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    bool isFullLocked() const noexcept {
        return ((head_ + 1) & kMask) == tail_;
    }

    bool isEmptyLocked() const noexcept {
        return head_ == tail_;
    }

    mutable std::mutex m_;
    std::condition_variable_any cvNotFull_;
    std::condition_variable_any cvNotEmpty_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    bool closed_ = false;
    // Own cache line: producers/consumers bump these on every operation, so
    // keep counter traffic off the mutex/head/tail lines.
    struct alignas(64) Counters {
        std::atomic<std::uint64_t> pushes{0};
        std::atomic<std::uint64_t> pops{0};
        std::atomic<std::uint64_t> pushWaits{0};
        std::atomic<std::uint64_t> popWaits{0};
        std::atomic<std::size_t> highWater{0};
    };
    Counters counters_;
    std::array<T, Capacity> storage_;
};

}  // namespace fqc::pipeline
