// =============================================================================
// fq-compressor - Reorder Buffer
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace fqc::pipeline {

/// In-order submission buffer for out-of-order-completed work.
///
/// Items keyed by a monotonically increasing `id` may arrive in any order
/// (e.g. N encoder workers finish frames at different times). `submit(id,
/// item)` buffers the item until the next expected id arrives, then returns
/// the contiguous run of items now ready to commit -- from `nextId` up to the
/// first gap. Callers drain that run in order, so downstream sees a strictly
/// ascending id sequence even though completion was unordered.
///
/// Single-threaded: the pipeline's writer thread owns the one instance, so no
/// internal locking. The pending window is bounded *externally* by the
/// upstream queue depth: once that queue fills, producers back-pressure, so
/// `pending_` cannot grow without bound and no separate window cap is needed
/// (see ARCHITECTURE.md, "execution" / "memory model").
template <typename T>
class ReorderBuffer {
public:
    /// Insert `item` keyed by `id`. Returns the contiguous run of items now
    /// ready to commit (possibly empty, if `id` left a gap). `id` must be
    /// monotonically non-decreasing across calls; an `id` already committed
    /// (`id < nextId_`) is a caller bug and is dropped.
    auto submit(std::uint64_t id, T item) -> std::vector<T> {
        std::vector<T> ready;
        if (id == nextId_) {
            ready.push_back(std::move(item));
            ++nextId_;
            drainContiguous(ready);
        } else if (id > nextId_) {
            pending_.emplace(id, std::move(item));
        }
        // id < nextId_: duplicate of an already-committed id; drop.
        return ready;
    }

    [[nodiscard]] auto pendingCount() const noexcept -> std::size_t {
        return pending_.size();
    }

    [[nodiscard]] auto nextId() const noexcept -> std::uint64_t {
        return nextId_;
    }

private:
    // Move out every buffered item that is now contiguous with nextId_.
    void drainContiguous(std::vector<T>& ready) {
        auto it = pending_.begin();
        while (it != pending_.end() && it->first == nextId_) {
            ready.push_back(std::move(it->second));
            ++nextId_;
            ++it;
        }
        pending_.erase(pending_.begin(), it);
    }

    std::uint64_t nextId_ = 0;
    std::map<std::uint64_t, T> pending_;
};

}  // namespace fqc::pipeline
