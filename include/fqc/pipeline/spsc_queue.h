#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace fqc::pipeline {

/// Single-producer single-consumer bounded ring buffer.
///
/// `close()` signals normal end-of-production: a consumer blocked on `pop()`
/// returns `std::nullopt` once the queue drains. `abort()` signals abnormal
/// shutdown: a producer blocked on a full `push()` returns `false` so it can
/// stop without deadlocking when the consumer has already failed.
///
/// Synchronization is a mutex plus two condition variables (`cvNotFull_` for
/// producers, `cvNotEmpty_` for consumers). This replaces an earlier
/// atomic + `std::this_thread::yield()` spin: the spin wasted CPU when one
/// side outran the other, and blocking on a CV lets the OS park the thread
/// until progress is possible. Because all state lives under the mutex, the
/// consumer observes a consistent snapshot, so the stale-head terminal recheck
/// the atomic version needed is no longer required.
template <typename T, std::size_t Capacity>
requires(Capacity > 0 && (Capacity & (Capacity - 1)) == 0)
class SpscQueue {
public:
    SpscQueue() = default;

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Enqueue an item. Returns false if the queue was aborted (item dropped);
    /// otherwise blocks until space is available and returns true.
    auto push(T item) -> bool {
        std::unique_lock lock(m_);
        cvNotFull_.wait(lock, [&] { return aborted_ || !isFullLocked(); });
        if (aborted_) {
            return false;
        }
        storage_[head_] = std::move(item);
        head_ = (head_ + 1) & kMask;
        lock.unlock();
        cvNotEmpty_.notify_one();
        return true;
    }

    /// Dequeue the next item, or `std::nullopt` if the queue is empty and has
    /// been closed or aborted.
    [[nodiscard]] auto pop() -> std::optional<T> {
        std::unique_lock lock(m_);
        cvNotEmpty_.wait(lock, [&] { return closed_ || aborted_ || !isEmptyLocked(); });
        if (isEmptyLocked()) {
            return std::nullopt;
        }
        T item = std::move(storage_[tail_]);
        tail_ = (tail_ + 1) & kMask;
        lock.unlock();
        cvNotFull_.notify_one();
        return item;
    }

    /// Signal that production is complete (no more items will be pushed).
    void close() {
        {
            std::lock_guard lock(m_);
            closed_ = true;
        }
        cvNotEmpty_.notify_all();
    }

    /// Signal abnormal shutdown: unblock a producer blocked on a full push and
    /// a consumer blocked on an empty pop.
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
    std::condition_variable cvNotFull_;
    std::condition_variable cvNotEmpty_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    bool closed_ = false;
    bool aborted_ = false;
    std::array<T, Capacity> storage_;
};

}  // namespace fqc::pipeline
