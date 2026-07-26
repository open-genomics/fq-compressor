#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace fqc::pipeline {

/// Single-producer single-consumer bounded ring buffer.
///
/// `close()` signals normal end-of-production: a consumer blocked on `pop()`
/// drains the remaining items, then returns `std::nullopt`. `abort()` signals
/// abnormal shutdown at the queue level (kept for unit tests): a producer
/// blocked on a full `push()` returns `false`, a consumer `std::nullopt`.
///
/// `push`/`pop` also accept a `std::stop_token`: when `request_stop()` is
/// called on the owning `stop_source`, a blocked `push` returns `false` and a
/// blocked `pop` returns `std::nullopt` *without draining* -- cooperative
/// cancellation that wakes the thread through the CV instead of spinning. This
/// is the cancel path the pipeline uses; `close()` remains the normal
/// end-of-production path with drain semantics.
///
/// Synchronization is a mutex plus two `condition_variable_any` (`cvNotFull_`
/// for producers, `cvNotEmpty_` for consumers). `condition_variable_any`
/// rather than `condition_variable` is required for the stop_token `wait`
/// overload that wakes on `request_stop()`. Because all state lives under the
/// mutex, the consumer observes a consistent snapshot.
template <typename T, std::size_t Capacity>
requires(Capacity > 0 && (Capacity & (Capacity - 1)) == 0)
class SpscQueue {
public:
    SpscQueue() = default;

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Enqueue an item. Returns false if `st` was stop-requested or the queue
    /// was aborted (item dropped); otherwise blocks until space is available
    /// and returns true.
    auto push(T item, std::stop_token st = {}) -> bool {
        std::unique_lock lock(m_);
        cvNotFull_.wait(
            lock, st, [&] { return st.stop_requested() || aborted_ || !isFullLocked(); });
        if (st.stop_requested() || aborted_) {
            return false;
        }
        storage_[head_] = std::move(item);
        head_ = (head_ + 1) & kMask;
        lock.unlock();
        cvNotEmpty_.notify_one();
        return true;
    }

    /// Dequeue the next item, or `std::nullopt`. Returns `std::nullopt` if `st`
    /// was stop-requested (cancel, without draining), or once the queue has
    /// drained after `close()`/`abort()`.
    [[nodiscard]] auto pop(std::stop_token st = {}) -> std::optional<T> {
        std::unique_lock lock(m_);
        cvNotEmpty_.wait(lock, st, [&] {
            return st.stop_requested() || closed_ || aborted_ || !isEmptyLocked();
        });
        if (st.stop_requested() || isEmptyLocked()) {
            return std::nullopt;
        }
        T item = std::move(storage_[tail_]);
        tail_ = (tail_ + 1) & kMask;
        lock.unlock();
        cvNotFull_.notify_one();
        return item;
    }

    /// Signal that production is complete (no more items will be pushed). A
    /// blocked consumer drains the remaining items, then sees `std::nullopt`.
    void close() {
        {
            std::lock_guard lock(m_);
            closed_ = true;
        }
        cvNotEmpty_.notify_all();
    }

    /// Signal abnormal shutdown: unblock a producer blocked on a full push and
    /// a consumer blocked on an empty pop. Kept for unit tests; the pipeline
    /// uses stop_token cancellation instead.
    void abort() {
        {
            std::lock_guard lock(m_);
            aborted_ = true;
        }
        cvNotFull_.notify_all();
        cvNotEmpty_.notify_all();
    }

    [[nodiscard]] auto isAborted() const -> bool {
        std::lock_guard lock(m_);
        return aborted_;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Caller must hold m_.
    bool isFullLocked() const noexcept {
        return ((head_ + 1) & kMask) == tail_;
    }

    // Caller must hold m_.
    bool isEmptyLocked() const noexcept {
        return head_ == tail_;
    }

    mutable std::mutex m_;
    std::condition_variable_any cvNotFull_;
    std::condition_variable_any cvNotEmpty_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    bool closed_ = false;
    bool aborted_ = false;
    std::array<T, Capacity> storage_;
};

}  // namespace fqc::pipeline
