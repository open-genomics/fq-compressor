// =============================================================================
// fq-compressor - Concurrent Compression Pipeline
// =============================================================================

#include "fqc/pipeline/compress_pipeline.h"

#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/frame_accumulator.h"
#include "fqc/pipeline/mpmc_queue.h"
#include "fqc/pipeline/reorder_buffer.h"
#include "fqc/pipeline/timing.h"

#include <atomic>
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
    MpmcQueue<InputFrame, kDefaultQueueDepth> queue1;
    MpmcQueue<OrderedFrame, kDefaultQueueDepth> queue2;
    std::optional<Error> readerError;
    std::optional<Error> encoderError;
    std::optional<Error> writerError;
    std::mutex encoderErrorMutex;
    PipelineStats stats;
    std::uint64_t logicalBytes = 0;

    // Per-stage wall-clock: locals merge once at exit with relaxed fetch_add
    // so clock reads stay off the synchronization hot path. Values are
    // advisory and never used for synchronization.
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
        FrameAccumulator accumulator(targetFrameBytes_, paired_);

        auto pushFrame = [&](std::vector<ReadRecord> closed) -> bool {
            InputFrame in{frameId++, std::move(closed)};
            const auto pushStart = Clock::now();
            bool ok = queue1.push(std::move(in), stopToken);
            pushNs += nanosSince(pushStart);
            return ok;
        };

        auto append = [&](ReadRecord record) -> bool {
            logicalBytes += canonicalFastqBytes(record);
            if (auto closed = accumulator.append(std::move(record))) {
                return pushFrame(std::move(*closed));
            }
            return true;
        };

        for (const auto& record : initialRecords) {
            if (stopToken.stop_requested()) {
                break;
            }
            if (!append(record)) {
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
            if (auto tail = accumulator.finish()) {
                pushFrame(std::move(*tail));
            }
        }
        queue1.close();
        readerParseNs.store(parseNs, std::memory_order_relaxed);
        readerPushNs.store(pushNs, std::memory_order_relaxed);
    });

    // N encoder workers: encodeFrame + compressFrame (no writer state).
    // Workers do not close queue2; the main thread closes it after join.
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
            auto compressed =
                format::compressFrame(std::move(*encoded), writer.options().qualityZstdLevel);
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

    // Writer: reorder by frameId, then assemble the frame header and write.
    // ArchiveWriter and the reorder buffer stay on this thread.
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
