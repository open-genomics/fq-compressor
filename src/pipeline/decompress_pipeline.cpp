// =============================================================================
// fq-compressor - Concurrent Decompression Pipeline
// =============================================================================

#include "fqc/pipeline/decompress_pipeline.h"

#include "fqc/format/archive.h"
#include "fqc/pipeline/in_flight_limiter.h"
#include "fqc/pipeline/mpmc_queue.h"
#include "fqc/pipeline/reorder_buffer.h"
#include "fqc/pipeline/stage_guard.h"
#include "fqc/pipeline/timing.h"
#include "fqc/pipeline/worker_stage.h"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace fqc::pipeline {

namespace {

/// A raw (still-compressed) frame queued from reader to a decoder worker.
/// `frameId` matches the on-disk frame order (the reader is the only
/// producer), so the writer's reorder buffer can restore submission order
/// after out-of-order decoding.
struct InputFrame {
    std::uint64_t frameId{};
    std::unique_ptr<format::RawFrame> frame;
};

/// A decoded frame queued from a decoder worker to the writer. unique_ptr so
/// the queue moves only the pointer -- the records vector of a large frame is
/// the single biggest allocation in the pipeline.
struct OrderedFrame {
    std::uint64_t frameId{};
    std::unique_ptr<format::DecodedFrame> frame;
};

}  // namespace

DecompressPipeline::DecompressPipeline(std::size_t maxFrameBytes,
                                       std::size_t memoryLimitBytes,
                                       std::size_t parallelism)
    : maxFrameBytes_(maxFrameBytes),
      memoryLimitBytes_(memoryLimitBytes),
      parallelism_(parallelism == 0 ? 1 : parallelism) {}

// reader/decoder/writer 的停止信号、队列关闭和 join 顺序属于同一生命周期，
// 拆分反而会隐藏同步不变量；复杂度来自阶段交错本身。
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto DecompressPipeline::run(std::istream& input,
                             RecordSink sink) const -> Result<DecompressStats> {
    MpmcQueue<InputFrame, kDefaultQueueDepth> queue1;
    MpmcQueue<OrderedFrame, kDefaultQueueDepth> queue2;
    const auto inFlightLimit = MpmcQueue<InputFrame, kDefaultQueueDepth>::kUsableCapacity +
        MpmcQueue<OrderedFrame, kDefaultQueueDepth>::kUsableCapacity + parallelism_;
    InFlightLimiter frameLimiter(inFlightLimit);
    std::optional<Error> readerError;
    std::optional<Error> decoderError;
    std::optional<Error> writerError;
    std::mutex decoderErrorMutex;

    // Synchronization invariant (same as CompressPipeline): each shared
    // variable below is written by exactly one worker and read by the main
    // thread only after the matching join(), whose happens-before edge makes
    // the access safe. The workers share no mutable state but the two queues
    // (MPMC, thread-safe) and the stop_source.
    DecompressStats stats;                  // writer
    std::uint64_t finalGlobalChecksum = 0;  // writer

    std::atomic<std::uint64_t> readerWorkNs{0};
    std::atomic<std::uint64_t> readerPushNs{0};
    WorkerStageTimingSink decoderTimings;
    std::atomic<std::uint64_t> writerPopNs{0};
    std::atomic<std::uint64_t> writerWorkNs{0};
    const auto wallStart = Clock::now();
    format::ArchiveMetadata metadata;                 // reader
    std::uint64_t encodedBytes = 0;                   // reader
    std::optional<format::ArchiveFooter> footerData;  // reader
    bool readerSawFooter = false;                     // reader

    std::stop_source stopSource;
    std::stop_token stopToken = stopSource.get_token();

    // Reader: open + readRawFrame. zstd, checksum, and record decode run on
    // decoder workers. records/bases and the rolling checksum accumulate on
    // the writer side.
    std::jthread reader([&] {
        std::uint64_t workNs = 0;
        std::uint64_t pushNs = 0;
        runGuarded("decompress reader", stopSource, readerError, [&] {
            const auto openStart = Clock::now();
            format::ArchiveReader archiveReader(input, maxFrameBytes_, memoryLimitBytes_);
            auto opened = archiveReader.open();
            workNs += nanosSince(openStart);
            if (!opened) {
                readerError = opened.error();
                stopSource.request_stop();
            } else {
                metadata = *opened;

                std::uint64_t frameId = 0;
                while (!stopToken.stop_requested()) {
                    const auto readStart = Clock::now();
                    auto raw = archiveReader.readRawFrame();
                    workNs += nanosSince(readStart);
                    if (!raw) {
                        readerError = raw.error();
                        stopSource.request_stop();
                        break;
                    }
                    if (!raw->has_value()) {
                        auto footer = archiveReader.footer();
                        if (!footer) {
                            readerError = footer.error();
                            stopSource.request_stop();
                            break;
                        }
                        footerData = *footer;
                        readerSawFooter = true;
                        break;
                    }
                    InputFrame in{frameId++, std::make_unique<format::RawFrame>(std::move(**raw))};
                    if (!frameLimiter.acquire(stopToken)) {
                        break;
                    }
                    const auto pushStart = Clock::now();
                    const bool pushed = queue1.push(std::move(in), stopToken);
                    pushNs += nanosSince(pushStart);
                    if (!pushed) {
                        frameLimiter.release();
                        break;
                    }
                }
                encodedBytes = archiveReader.stats().encodedBytes;
            }
        });
        queue1.close();
        readerWorkNs.store(workNs, std::memory_order_relaxed);
        readerPushNs.store(pushNs, std::memory_order_relaxed);
    });

    // N decoder workers: decodeRawFrame is pure computation. Workers do not
    // close queue2; the main thread closes it after join.
    auto decodeWorker = [&](const InputFrame& in,
                            WorkerStageTimings& timings) -> Result<OrderedFrame> {
        const auto decodeStart = Clock::now();
        auto decoded = format::decodeRawFrame(*in.frame);
        timings.computeNs += nanosSince(decodeStart);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        return OrderedFrame{in.frameId,
                            std::make_unique<format::DecodedFrame>(std::move(*decoded))};
    };
    auto decoders = startWorkerStage(parallelism_,
                                     queue1,
                                     queue2,
                                     stopToken,
                                     stopSource,
                                     decoderErrorMutex,
                                     decoderError,
                                     "decompress decoder",
                                     std::move(decodeWorker),
                                     decoderTimings);

    // Writer: reorder by frameId, then accumulate stats + rolling global
    // checksum (order-dependent -- must stay on this thread) and invoke the
    // sink. A sink failure cancels the pipeline.
    std::jthread writer([&] {
        std::uint64_t popNs = 0;
        std::uint64_t workNs = 0;
        runGuarded("decompress writer", stopSource, writerError, [&] {
            ReorderBuffer<std::unique_ptr<format::DecodedFrame>> reorder;
            while (!stopToken.stop_requested()) {
                const auto popStart = Clock::now();
                auto out = queue2.pop(stopToken);
                popNs += nanosSince(popStart);
                if (!out.has_value()) {
                    break;
                }
                auto ready = reorder.submit(out->frameId, std::move(out->frame));
                for (auto& frame : ready) {
                    const auto workStart = Clock::now();
                    stats.archive.frameCount += 1;
                    stats.archive.recordCount += frame->recordCount;
                    stats.archive.totalBases += frame->totalBases;
                    for (const auto& record : frame->records) {
                        stats.logicalBytes += canonicalFastqBytes(record);
                    }
                    finalGlobalChecksum =
                        format::advanceGlobalChecksum(finalGlobalChecksum, frame->checksum);
                    auto sinkResult = sink(std::move(frame->records));
                    workNs += nanosSince(workStart);
                    frameLimiter.release();
                    if (!sinkResult) {
                        writerError = sinkResult.error();
                        stopSource.request_stop();
                        break;
                    }
                }
                if (writerError.has_value()) {
                    break;
                }
            }
        });
        writerPopNs.store(popNs, std::memory_order_relaxed);
        writerWorkNs.store(workNs, std::memory_order_relaxed);
    });

    reader.join();
    for (auto& decoder : decoders) {
        decoder.join();
    }
    // All decoders have stopped producing; signal end-of-stream so the writer
    // drains queue2 and exits instead of blocking on an empty pop.
    queue2.close();
    writer.join();

    stats.telemetry.timings = {
        .readerWorkNs = readerWorkNs.load(std::memory_order_relaxed),
        .readerPushNs = readerPushNs.load(std::memory_order_relaxed),
        .workerPopNs = decoderTimings.popNs.load(std::memory_order_relaxed),
        .workerPrimaryNs = decoderTimings.computeNs.load(std::memory_order_relaxed),
        .workerSecondaryNs = 0,
        .workerPushNs = decoderTimings.pushNs.load(std::memory_order_relaxed),
        .writerPopNs = writerPopNs.load(std::memory_order_relaxed),
        .writerWorkNs = writerWorkNs.load(std::memory_order_relaxed),
        .wallNs = nanosSince(wallStart),
    };
    stats.telemetry.inputQueueStats = queue1.stats();
    stats.telemetry.outputQueueStats = queue2.stats();
    stats.telemetry.inFlightHighWater = frameLimiter.highWater();

    if (writerError.has_value()) {
        return makeError<DecompressStats>(std::move(*writerError));
    }
    if (decoderError.has_value()) {
        return makeError<DecompressStats>(std::move(*decoderError));
    }
    if (readerError.has_value()) {
        return makeError<DecompressStats>(std::move(*readerError));
    }
    if (!readerSawFooter) {
        // A clean run can only end at the footer; anything else means the
        // archive was cut short without a lower-level error surfacing.
        return makeError<DecompressStats>(ErrorCode::kFormatError,
                                          "FQC v2 archive ended before the footer");
    }

    // Footer validation: totals and the rolling global checksum are checked
    // against the writer side's accumulated (in-order) values -- same
    // strictness as the sequential ArchiveReader path.
    if (footerData->frameCount != stats.archive.frameCount ||
        footerData->recordCount != stats.archive.recordCount ||
        footerData->totalBases != stats.archive.totalBases) {
        return makeError<DecompressStats>(ErrorCode::kFormatError, "FQC v2 footer totals disagree");
    }
    if (footerData->globalChecksum != finalGlobalChecksum) {
        return makeError<DecompressStats>(ErrorCode::kChecksumError,
                                          "FQC v2 global checksum mismatch");
    }

    stats.metadata = metadata;
    stats.archive.encodedBytes = encodedBytes;
    stats.decoderWorkers = parallelism_;
    return stats;
}

}  // namespace fqc::pipeline
