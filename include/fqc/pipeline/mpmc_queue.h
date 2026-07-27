// =============================================================================
// fq-compressor - MPMC Bounded Queue
// =============================================================================

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace fqc::pipeline {

/// Multi-producer multi-consumer bounded ring buffer.
///
/// A mutex plus two `condition_variable_any` serializes all producers and
/// consumers, so the queue is safe for concurrent fan-in and fan-out (stage D
/// fans N encoders onto one queue and merges them onto another).
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

    /// Enqueue an item. Returns false if `st` was stop-requested; otherwise
    /// blocks until space is available and returns true. Safe for concurrent
    /// producers.
    auto push(T item, std::stop_token st = {}) -> bool {
        std::unique_lock lock(m_);
        cvNotFull_.wait(lock, st, [&] { return st.stop_requested() || !isFullLocked(); });
        if (st.stop_requested()) {
            return false;
        }
        storage_[head_] = std::move(item);
        head_ = (head_ + 1) & kMask;
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
        cvNotEmpty_.wait(
            lock, st, [&] { return st.stop_requested() || closed_ || !isEmptyLocked(); });
        if (st.stop_requested() || isEmptyLocked()) {
            return std::nullopt;
        }
        T item = std::move(storage_[tail_]);
        tail_ = (tail_ + 1) & kMask;
        lock.unlock();
        cvNotFull_.notify_one();
        return item;
    }

    /// Signal that production is complete (no more items will be pushed). All
    /// blocked consumers wake, drain the remaining items, then see `nullopt`.
    void close() {
        {
            std::lock_guard lock(m_);
            closed_ = true;
        }
        cvNotEmpty_.notify_all();
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
    std::array<T, Capacity> storage_;
};

}  // namespace fqc::pipeline
