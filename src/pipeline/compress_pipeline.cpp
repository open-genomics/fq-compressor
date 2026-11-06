// =============================================================================
// fq-compressor - Concurrent Compression Pipeline
// =============================================================================

#include "fqc/pipeline/compress_pipeline.h"

#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/frame_accumulator.h"
#include "fqc/pipeline/in_flight_limiter.h"
#include "fqc/pipeline/mpmc_queue.h"
#include "fqc/pipeline/reorder_buffer.h"
#include "fqc/pipeline/stage_guard.h"
#include "fqc/pipeline/timing.h"
#include "fqc/pipeline/worker_stage.h"

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
    std::uint64_t frameId{};
    std::vector<ReadRecord> records;
};

/// A compressed frame queued from an encoder worker to the writer. Carries
/// the same `frameId` so the writer's reorder buffer can submit frames to
/// `writeCompressedFrame` in ascending id order. The on-disk frame id is
/// derived from the writer's own frame counter, so in-order submission keeps
/// the archive's monotonic-frame-id invariant (see ARCHITECTURE.md).
struct OrderedFrame {
    std::uint64_t frameId{};
    std::unique_ptr<format::CompressedFrame> frame;
};

}  // namespace

CompressPipeline::CompressPipeline(std::size_t targetFrameBytes,
                                   bool paired,
                                   std::size_t parallelism)
    : targetFrameBytes_(targetFrameBytes),
      paired_(paired),
      parallelism_(parallelism == 0 ? 1 : parallelism) {}

// 有界管线刻意顺序化各阶段的共享缓冲与停止信号，拆分会破坏其可见性；
// 复杂度来自阶段交错本身，而非可消除的分支。
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto CompressPipeline::run(std::istream& primary,
                           std::istream* mate,
                           std::span<const ReadRecord> initialRecords,
                           format::ArchiveWriter& writer) const -> Result<PipelineStats> {
    MpmcQueue<InputFrame, kDefaultQueueDepth> queue1;
    MpmcQueue<OrderedFrame, kDefaultQueueDepth> queue2;
    const auto inFlightLimit = MpmcQueue<InputFrame, kDefaultQueueDepth>::kUsableCapacity +
        MpmcQueue<OrderedFrame, kDefaultQueueDepth>::kUsableCapacity + parallelism_;
    InFlightLimiter frameLimiter(inFlightLimit);
    // 全流水线唯一的首个错误槽：reader / encoder / writer 都可能并发失败，
    // 所有写入都走同一把错误锁；正常路径不加锁。只保留第一个失败，避免固定
    // 阶段优先级掩盖真正的根因。
    std::optional<Error> firstError;
    std::mutex errorMutex;
    PipelineStats stats;
    stats.parserWorkers = 1;
    stats.encoderWorkers = parallelism_;
    std::uint64_t logicalBytes = 0;

    // Per-stage wall-clock: locals merge once at exit with relaxed fetch_add
    // so clock reads stay off the synchronization hot path. Values are
    // advisory and never used for synchronization.
    const auto wallStart = Clock::now();
    std::atomic<std::uint64_t> readerParseNs{0};
    std::atomic<std::uint64_t> readerPushNs{0};
    WorkerStageTimingSink encoderTimings;
    std::atomic<std::uint64_t> writerPopNs{0};
    std::atomic<std::uint64_t> writerWriteNs{0};

    // Cooperative cancellation. Any stage, on failure, calls request_stop();
    // the other stages' blocked push/pop wakes through the stop_token CV wait
    // and exits. Normal end-of-production chains: reader closes queue1, each
    // encoder sees nullopt and exits; once all encoders have joined, the main
    // thread closes queue2 and the writer drains it then sees nullopt.
    std::stop_source stopSource;
    std::stop_token stopToken = stopSource.get_token();

    // Synchronization invariant: `logicalBytes` is written by the reader and
    // `stats` by the writer; the main thread reads them only after the
    // matching join() below, whose happens-before edge makes the access safe.
    // `firstError` is the one cross-stage slot: every stage writes it under
    // `errorMutex`, so the first failure wins regardless of which stage
    // reports it. Otherwise the workers share only the two queues (MPMC,
    // thread-safe) and the stop_source.
    std::jthread reader([&] {
        std::uint64_t parseNs = 0;
        std::uint64_t pushNs = 0;
        runGuarded("compress reader", stopSource, firstError, errorMutex, [&] {
            io::FastqParser parser(primary);
            std::optional<io::FastqParser> mateParser;
            if (mate != nullptr) {
                mateParser.emplace(*mate);
            }

            std::uint64_t frameId = 0;
            FrameAccumulator accumulator(targetFrameBytes_, paired_);

            auto pushFrame = [&](std::vector<ReadRecord> closed) -> bool {
                const auto pushStart = Clock::now();
                const bool ok = pushWithCredit(
                    frameLimiter, queue1, InputFrame{frameId++, std::move(closed)}, stopToken);
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
                    recordGuardedError(errorMutex, firstError, pair.error());
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
        });
        queue1.close();
        readerParseNs.store(parseNs, std::memory_order_relaxed);
        readerPushNs.store(pushNs, std::memory_order_relaxed);
    });

    // N encoder workers: encodeFrame + compressFrame (no writer state).
    // Workers do not close queue2; the main thread closes it after join.
    auto encodeWorker = [&](const InputFrame& in,
                            WorkerStageTimings& timings) -> Result<OrderedFrame> {
        const auto encodeStart = Clock::now();
        auto encoded = format::encodeFrame(in.records, writer.options());
        timings.computeNs += nanosSince(encodeStart);
        if (!encoded) {
            return std::unexpected(std::move(encoded.error()));
        }
        const auto compressStart = Clock::now();
        auto compressed =
            format::compressFrame(std::move(*encoded), writer.options().qualityZstdLevel);
        timings.secondaryNs += nanosSince(compressStart);
        if (!compressed) {
            return std::unexpected(std::move(compressed.error()));
        }
        return OrderedFrame{in.frameId, std::move(*compressed)};
    };
    auto encoders = startWorkerStage(parallelism_,
                                     queue1,
                                     queue2,
                                     stopToken,
                                     stopSource,
                                     errorMutex,
                                     firstError,
                                     "compress encoder",
                                     std::move(encodeWorker),
                                     encoderTimings);

    // Writer: reorder by frameId, then assemble the frame header and write.
    // ArchiveWriter and the reorder buffer stay on this thread.
    std::jthread writerThread([&] {
        std::uint64_t popNs = 0;
        std::uint64_t writeNs = 0;
        runGuarded("compress writer", stopSource, firstError, errorMutex, [&] {
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
                    const auto writeStart = Clock::now();
                    auto result = writer.writeCompressedFrame(std::move(frame));
                    writeNs += nanosSince(writeStart);
                    frameLimiter.release();
                    if (!result) {
                        recordGuardedError(errorMutex, firstError, result.error());
                        stopSource.request_stop();
                        return;
                    }
                }
                // 其它阶段失败时 stopToken 已请求，下一轮 while 条件会退出；
                // 本阶段失败则由上面的 return 直接结束 writer lambda。
            }
        });
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

    if (firstError.has_value()) {
        return makeError<PipelineStats>(std::move(*firstError));
    }
    // A compression run owns the archive lifetime: finish the footer before
    // exposing counters, so `PipelineStats.archive` is a complete snapshot.
    FQC_TRY(writer.finish());

    // All workers joined, so the relaxed loads below see the final values.
    stats.telemetry = makeTelemetry(
        {
            .readerWorkNs = readerParseNs.load(std::memory_order_relaxed),
            .readerPushNs = readerPushNs.load(std::memory_order_relaxed),
            .workerPopNs = encoderTimings.popNs.load(std::memory_order_relaxed),
            .workerPrimaryNs = encoderTimings.computeNs.load(std::memory_order_relaxed),
            .workerSecondaryNs = encoderTimings.secondaryNs.load(std::memory_order_relaxed),
            .workerPushNs = encoderTimings.pushNs.load(std::memory_order_relaxed),
            .writerPopNs = writerPopNs.load(std::memory_order_relaxed),
            .writerWorkNs = writerWriteNs.load(std::memory_order_relaxed),
            .wallNs = nanosSince(wallStart),
        },
        queue1,
        queue2,
        frameLimiter);
    stats.metadata = writer.metadata();
    // ArchiveWriter owns the authoritative byte/record counters; copying them
    // after join keeps the pipeline result aligned with what was actually
    // written instead of maintaining a second set of counters in parallel.
    stats.archive = writer.stats();
    stats.logicalBytes = logicalBytes;
    return stats;
}

}  // namespace fqc::pipeline
