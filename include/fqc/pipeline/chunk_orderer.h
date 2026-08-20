// =============================================================================
// fq-compressor - Two-Level (Chunk, Local) Reorder Buffer
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace fqc::pipeline {

/// Reorders out-of-order completions into (chunkId, localId) lexicographic
/// order -- the ordered-submission core of parallel parsing.
///
/// Producers are K chunk workers, each tagging its frames with a chunk-local
/// monotonic id; items from different chunks interleave arbitrarily. A chunk
/// is drained only after (a) every one of its frames was emitted in local
/// order AND (b) its completion marker (carrying the chunk's total frame
/// count) arrived. Markers may overtake frames of the same chunk (different
/// workers forward them), so a chunk advances only when the next expected
/// local id equals the marker's total.
///
/// Generalizes ReorderBuffer's single monotonic sequence to two levels; the
/// writer thread owns it (single consumer, no locking). Backpressure comes
/// from the bounded upstream queues, exactly like ReorderBuffer.
template <typename T>
class ChunkOrderer {
public:
    /// Submit one frame. Returns the longest ready sequence in (chunk, local)
    /// order (possibly empty).
    [[nodiscard]] auto submitFrame(std::uint64_t chunk,
                                   std::uint64_t local,
                                   T value) -> std::vector<T> {
        pending_.emplace(std::make_pair(chunk, local), std::move(value));
        return drain();
    }

    /// Submit a chunk-completion marker: `totalFrames` is how many frames the
    /// chunk produced (0 allowed). Returns any items unblocked by it.
    [[nodiscard]] auto submitChunkEnd(std::uint64_t chunk,
                                      std::uint64_t totalFrames) -> std::vector<T> {
        chunkTotals_.emplace(chunk, totalFrames);
        return drain();
    }

private:
    [[nodiscard]] auto drain() -> std::vector<T> {
        std::vector<T> ready;
        while (true) {
            auto frameIt = pending_.find(std::make_pair(nextChunk_, nextLocal_));
            if (frameIt != pending_.end()) {
                ready.push_back(std::move(frameIt->second));
                pending_.erase(frameIt);
                ++nextLocal_;
                continue;
            }
            // No pending frame at the cursor: the chunk can only advance once
            // its marker says every frame was already emitted.
            auto totalIt = chunkTotals_.find(nextChunk_);
            if (totalIt != chunkTotals_.end() && nextLocal_ == totalIt->second) {
                chunkTotals_.erase(totalIt);
                ++nextChunk_;
                nextLocal_ = 0;
                continue;
            }
            break;
        }
        return ready;
    }

    std::map<std::pair<std::uint64_t, std::uint64_t>, T> pending_;
    std::map<std::uint64_t, std::uint64_t> chunkTotals_;
    std::uint64_t nextChunk_ = 0;
    std::uint64_t nextLocal_ = 0;
};

}  // namespace fqc::pipeline
