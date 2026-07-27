// =============================================================================
// fq-compressor - Concurrent Compression Pipeline
// =============================================================================

#include "fqc/pipeline/compress_pipeline.h"

#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/mpmc_queue.h"
#include "fqc/pipeline/reorder_buffer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace fqc::pipeline {

namespace {

[[nodiscard]] auto retainedRecordBytes(const ReadRecord& record) noexcept -> std::size_t {
    return sizeof(ReadRecord) + record.id.capacity() + 1 + record.comment.capacity() + 1 +
        record.sequence.capacity() + 1 + record.quality.capacity() + 1;
}

/// A parsed frame queued from reader to an encoder worker. `frameId` is the
/// monotonic order in which the reader closed the frame; encoders complete out
/// of order, so the id rides along to drive the reorder buffer.
struct InputFrame {
    std::uint64_t frameId;
    std::vector<ReadRecord> records;
};

/// A compressed frame queued from an encoder worker to the writer. Carries
/// the same `frameId` so the writer's reorder buffer can submit frames to
/// `writeCompressedFrame` in ascending id order. The on-disk frame id is
/// derived from the writer's own frame counter, so in-order submission keeps
/// the archive's monotonic-frame-id invariant (see ARCHITECTURE.md).
struct OrderedFrame {
    std::uint64_t frameId;
    std::unique_ptr<format::CompressedFrame> frame;
};

using Clock = std::chrono::steady_clock;

[[nodiscard]] auto nanosSince(Clock::time_point start) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

}  // namespace

CompressPipeline::CompressPipeline(std::size_t targetFrameBytes,
                                   bool paired,
                                   std::size_t parallelism)
    : targetFrameBytes_(targetFrameBytes),
      paired_(paired),
      parallelism_(parallelism == 0 ? 1 : parallelism) {}

auto CompressPipeline::run(std::istream& primary,
                           std::istream* mate,
                           std::span<const ReadRecord> initialRecords,
                           format::ArchiveWriter& writer) -> Result<PipelineStats> {
    // Stage 1 -> 2: parsed FASTQ records accumulated into bounded frames,
    // each tagged with a monotonic frame id for the reorder buffer.
    MpmcQueue<InputFrame, kDefaultQueueDepth> queue1;
    // Stage 2 -> 3: compressed frames. unique_ptr so the queue moves only the
    // frame pointer; OrderedFrame carries the id for reorder. The queue2
    // payload shrank from raw streams (stage D) to compressed streams (stage
    // F), so its in-flight footprint only got smaller.
    MpmcQueue<OrderedFrame, kDefaultQueueDepth> queue2;
    std::optional<Error> readerError;
    std::optional<Error> encoderError;
    std::optional<Error> writerError;
    std::mutex encoderErrorMutex;
    PipelineStats stats;
    std::uint64_t logicalBytes = 0;

    // Stage-E observability: per-stage wall-clock accumulators. Each thread
    // collects `steady_clock` samples in locals and merges once at exit with a
    // relaxed fetch_add (N encoders => N merges total), keeping clock reads
    // out of the synchronization hot path. Relaxed is sufficient: the values
    // are advisory stats, never used for synchronization.
    const auto wallStart = Clock::now();
    std::atomic<std::uint64_t> readerParseNs{0};
    std::atomic<std::uint64_t> readerPushNs{0};
    std::atomic<std::uint64_t> encoderPopNs{0};
    std::atomic<std::uint64_t> encoderEncodeNs{0};
    std::atomic<std::uint64_t> encoderCompressNs{0};
    std::atomic<std::uint64_t> encoderPushNs{0};
    std::atomic<std::uint64_t> writerPopNs{0};
    std::atomic<std::uint64_t> writerWriteNs{0};

    // Cooperative cancellation. Any stage, on failure, calls request_stop();
    // the other stages' blocked push/pop wakes through the stop_token CV wait
    // and exits. Normal end-of-production chains: reader closes queue1, each
    // encoder sees nullopt and exits; once all encoders have joined, the main
    // thread closes queue2 and the writer drains it then sees nullopt.
    std::stop_source stopSource;
    std::stop_token stopToken = stopSource.get_token();

    // Synchronization invariant: each shared variable is written by exactly
    // one worker -- `readerError`/`logicalBytes` by reader, `writerError`/
    // `stats` by the writer, `encoderError` by whichever encoder fails first
    // (guarded by `encoderErrorMutex` since N encoders compete). The workers
    // share no mutable state but the two queues (MPMC, thread-safe) and the
    // stop_source. The main thread reads these only after the matching join()
    // below, whose happens-before edge makes the access safe.
    std::jthread reader([&] {
        std::uint64_t parseNs = 0;
        std::uint64_t pushNs = 0;

        io::FastqParser parser(primary);
        std::optional<io::FastqParser> mateParser;
        if (mate != nullptr) {
            mateParser.emplace(*mate);
        }

        std::uint64_t frameId = 0;
        std::vector<ReadRecord> frame;
        std::size_t retainedBytes = 0;

        auto frameFull = [&] {
            return retainedBytes >= targetFrameBytes_ && (!paired_ || frame.size() % 2 == 0);
        };

        auto pushFrame = [&] -> bool {
            if (frame.empty()) {
                return true;
            }
            InputFrame in{frameId++, std::move(frame)};
            const auto pushStart = Clock::now();
            bool ok = queue1.push(std::move(in), stopToken);
            pushNs += nanosSince(pushStart);
            frame.clear();
            retainedBytes = 0;
            return ok;
        };

        auto append = [&](ReadRecord record) -> bool {
            logicalBytes += canonicalFastqBytes(record);
            retainedBytes += retainedRecordBytes(record);
            frame.push_back(std::move(record));
            if (frameFull()) {
                return pushFrame();
            }
            return true;
        };

        for (auto& record : initialRecords) {
            if (stopToken.stop_requested()) {
                break;
            }
            if (!append(std::move(record))) {
                break;
            }
        }

        while (!stopToken.stop_requested()) {
            const auto parseStart = Clock::now();
            auto pair = io::readRecordPair(parser, mateParser ? &*mateParser : nullptr);
            parseNs += nanosSince(parseStart);
            if (!pair) {
                readerError = pair.error();
                stopSource.request_stop();
                break;
            }
            if (!pair->has_value()) {
                break;
            }
            if (!append(std::move((*pair)->first))) {
                break;
            }
            if ((*pair)->second) {
                if (!append(std::move(*(*pair)->second))) {
                    break;
                }
            }
        }

        if (!stopToken.stop_requested()) {
            pushFrame();
        }
        queue1.close();
        readerParseNs.store(parseNs, std::memory_order_relaxed);
        readerPushNs.store(pushNs, std::memory_order_relaxed);
    });

    // Stage 2: N encoder workers compete for frames off queue1, encode AND
    // zstd-compress each (CPU-only, parallel), and push the result to queue2.
    // `encodeFrame`/`compressFrame` are free functions touching no writer
    // state, so N workers are safe (stage F: compression moved here from the
    // writer, where it serialized on a single thread). Workers do not close
    // queue2 -- multiple producers mean the main thread closes it once every
    // encoder has joined.
    auto encoderLoop = [&] {
        std::uint64_t popNs = 0;
        std::uint64_t encodeNs = 0;
        std::uint64_t compressNs = 0;
        std::uint64_t pushNs = 0;
        while (!stopToken.stop_requested()) {
            const auto popStart = Clock::now();
            auto in = queue1.pop(stopToken);
            popNs += nanosSince(popStart);
            if (!in.has_value()) {
                break;
            }
            const auto encodeStart = Clock::now();
            auto encoded = format::encodeFrame(in->records, writer.options());
            encodeNs += nanosSince(encodeStart);
            if (!encoded) {
                {
                    std::lock_guard lk(encoderErrorMutex);
                    if (!encoderError) {
                        encoderError = encoded.error();
                    }
                }
                stopSource.request_stop();
                break;
            }
            const auto compressStart = Clock::now();
            auto compressed = format::compressFrame(std::move(*encoded));
            compressNs += nanosSince(compressStart);
            if (!compressed) {
                {
                    std::lock_guard lk(encoderErrorMutex);
                    if (!encoderError) {
                        encoderError = compressed.error();
                    }
                }
                stopSource.request_stop();
                break;
            }
            OrderedFrame out{in->frameId, std::move(*compressed)};
            const auto pushStart = Clock::now();
            const bool pushed = queue2.push(std::move(out), stopToken);
            pushNs += nanosSince(pushStart);
            if (!pushed) {
                break;
            }
        }
        encoderPopNs.fetch_add(popNs, std::memory_order_relaxed);
        encoderEncodeNs.fetch_add(encodeNs, std::memory_order_relaxed);
        encoderCompressNs.fetch_add(compressNs, std::memory_order_relaxed);
        encoderPushNs.fetch_add(pushNs, std::memory_order_relaxed);
    };

    std::vector<std::jthread> encoders;
    encoders.reserve(parallelism_);
    for (std::size_t i = 0; i < parallelism_; ++i) {
        encoders.emplace_back(encoderLoop);
    }

    // Stage 3: pop compressed frames, reorder by frameId, assemble the frame
    // header + write to disk + update stats/checksum. All CPU-heavy work
    // (encode + zstd) happens upstream in the worker pool, so this thread is
    // I/O-bound by design. ArchiveWriter and the reorder buffer are owned by
    // this single thread -- no race on output_/stats_/checksum.
    std::jthread writerThread([&] {
        std::uint64_t popNs = 0;
        std::uint64_t writeNs = 0;
        ReorderBuffer<std::unique_ptr<format::CompressedFrame>> reorder;
        while (!stopToken.stop_requested()) {
            const auto popStart = Clock::now();
            auto out = queue2.pop(stopToken);
            popNs += nanosSince(popStart);
            if (!out.has_value()) {
                break;
            }
            auto ready = reorder.submit(out->frameId, std::move(out->frame));
            for (auto& frame : ready) {
                stats.recordCount += frame->recordCount;
                stats.frameCount += 1;
                const auto writeStart = Clock::now();
                auto result = writer.writeCompressedFrame(std::move(frame));
                writeNs += nanosSince(writeStart);
                if (!result) {
                    writerError = result.error();
                    stopSource.request_stop();
                    break;
                }
            }
            if (writerError.has_value()) {
                break;
            }
        }
        writerPopNs.store(popNs, std::memory_order_relaxed);
        writerWriteNs.store(writeNs, std::memory_order_relaxed);
    });

    reader.join();
    for (auto& encoder : encoders) {
        encoder.join();
    }
    // All encoders have stopped producing; signal end-of-stream so the writer
    // drains queue2 and exits instead of blocking on an empty pop.
    queue2.close();
    writerThread.join();

    // All workers joined, so the relaxed loads below see the final values.
    stats.timings = {
        .readerParseNs = readerParseNs.load(std::memory_order_relaxed),
        .readerPushNs = readerPushNs.load(std::memory_order_relaxed),
        .encoderPopNs = encoderPopNs.load(std::memory_order_relaxed),
        .encoderEncodeNs = encoderEncodeNs.load(std::memory_order_relaxed),
        .encoderCompressNs = encoderCompressNs.load(std::memory_order_relaxed),
        .encoderPushNs = encoderPushNs.load(std::memory_order_relaxed),
        .writerPopNs = writerPopNs.load(std::memory_order_relaxed),
        .writerWriteNs = writerWriteNs.load(std::memory_order_relaxed),
        .wallNs = nanosSince(wallStart),
    };
    stats.queue1Stats = queue1.stats();
    stats.queue2Stats = queue2.stats();
    stats.logicalBytes = logicalBytes;

    if (writerError.has_value()) {
        return makeError<PipelineStats>(std::move(*writerError));
    }
    if (encoderError.has_value()) {
        return makeError<PipelineStats>(std::move(*encoderError));
    }
    if (readerError.has_value()) {
        return makeError<PipelineStats>(std::move(*readerError));
    }
    return stats;
}

}  // namespace fqc::pipeline
