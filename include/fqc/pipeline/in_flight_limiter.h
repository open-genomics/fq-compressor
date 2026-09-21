// =============================================================================
// fq-compressor - In-Flight Frame Limiter
// =============================================================================

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <vector>

namespace fqc::pipeline {

/// Limits the number of frames owned by a pipeline after source submission.
///
/// The limiter is intentionally separate from the inter-stage queues: a writer
/// may remove out-of-order frames from the final queue and hold them in a
/// reorder buffer, so queue capacity alone cannot bound total in-flight work.
/// A source acquires one credit before enqueueing a frame; the ordered sink
/// releases it after committing that frame.
class InFlightLimiter {
public:
    /// `reservedOwners` gives each owner in `[0, reservedOwners)` one credit
    /// that other owners cannot consume. It must not exceed `capacity`.
    /// Owner credits must be released in the same order they were acquired;
    /// a per-owner ordered sink naturally provides that property.
    explicit InFlightLimiter(std::size_t capacity, std::size_t reservedOwners = 0)
        : capacity_(capacity),
          sharedCapacity_(capacity - reservedOwners),
          reservedInUse_(reservedOwners, false) {}

    InFlightLimiter(const InFlightLimiter&) = delete;
    InFlightLimiter& operator=(const InFlightLimiter&) = delete;

    /// Acquire one credit, or return false when cancellation is requested.
    [[nodiscard]] auto acquire(std::stop_token st = {}) -> bool {
        return acquire(kNoOwner, st);
    }

    /// Acquire one credit for `owner`, keeping one credit available to each
    /// owner even when the shared portion of the window is full.
    [[nodiscard]] auto acquire(std::size_t owner, std::stop_token st = {}) -> bool {
        std::unique_lock lock(m_);
        cv_.wait(lock, st, [&] { return st.stop_requested() || canAcquireLocked(owner); });
        if (st.stop_requested()) {
            return false;
        }
        if (owner != kNoOwner && !reservedInUse_[owner]) {
            reservedInUse_[owner] = true;
        } else {
            ++sharedInFlight_;
        }
        ++inFlight_;
        if (inFlight_ > highWater_) {
            highWater_ = inFlight_;
        }
        return true;
    }

    /// Release one credit after the frame has reached the ordered sink.
    void release() {
        release(kNoOwner);
    }

    /// Release one owner credit after the frame has reached the ordered sink.
    void release(std::size_t owner) {
        bool reservedReleased = false;
        {
            const std::lock_guard lock(m_);
            if (owner != kNoOwner && reservedInUse_[owner]) {
                reservedInUse_[owner] = false;
                reservedReleased = true;
            } else {
                --sharedInFlight_;
            }
            --inFlight_;
        }
        if (reservedReleased) {
            // Each waiter has a different owner-specific predicate; notify_one
            // could repeatedly wake only owners whose reserved slot is busy.
            cv_.notify_all();
        } else {
            cv_.notify_one();
        }
    }

    /// Return the largest number of simultaneously acquired credits.
    [[nodiscard]] auto highWater() const -> std::size_t {
        const std::lock_guard lock(m_);
        return highWater_;
    }

private:
    static constexpr std::size_t kNoOwner = static_cast<std::size_t>(-1);

    [[nodiscard]] auto canAcquireLocked(std::size_t owner) const -> bool {
        return inFlight_ < capacity_ &&
            (owner != kNoOwner && !reservedInUse_[owner] ? true
                                                         : sharedInFlight_ < sharedCapacity_);
    }

    const std::size_t capacity_;
    const std::size_t sharedCapacity_;
    mutable std::mutex m_;
    std::condition_variable_any cv_;
    std::size_t inFlight_ = 0;
    std::size_t sharedInFlight_ = 0;
    std::size_t highWater_ = 0;
    std::vector<bool> reservedInUse_;
};

}  // namespace fqc::pipeline
