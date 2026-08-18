// =============================================================================
// fq-compressor - Concurrent Decompression Pipeline
// =============================================================================

#include "fqc/pipeline/decompress_pipeline.h"

#include "fqc/format/archive.h"
#include "fqc/pipeline/mpmc_queue.h"
#include "fqc/pipeline/reorder_buffer.h"

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
    std::uint64_t frameId;
    std::unique_ptr<format::RawFrame> frame;
};

/// A decoded frame queued from a decoder worker to the writer. unique_ptr so
/// the queue moves only the pointer -- the records vector of a large frame is
/// the single biggest allocation in the pipeline.
struct OrderedFrame {
    std::uint64_t frameId;
    std::unique_ptr<format::DecodedFrame> frame;
};

}  // namespace

DecompressPipeline::DecompressPipeline(std::size_t maxFrameBytes,
                                       std::size_t memoryLimitBytes,
                                       std::size_t parallelism)
    : maxFrameBytes_(maxFrameBytes),
      memoryLimitBytes_(memoryLimitBytes),
      parallelism_(parallelism == 0 ? 1 : parallelism) {}

auto DecompressPipeline::run(std::istream& input, RecordSink sink) -> Result<DecompressStats> {
    MpmcQueue<InputFrame, kDefaultQueueDepth> queue1;
    MpmcQueue<OrderedFrame, kDefaultQueueDepth> queue2;
    std::optional<Error> readerError;
    std::optional<Error> decoderError;
    std::optional<Error> writerError;
    std::mutex decoderErrorMutex;

    // Synchronization invariant (same as CompressPipeline): each shared
    // variable below is written by exactly one worker and read by the main
    // thread only after the matching join(), whose happens-before edge makes
    // the access safe. The workers share no mutable state but the two queues
    // (MPMC, thread-safe) and the stop_source.
    DecompressStats stats;                            // writer
    std::uint64_t finalGlobalChecksum = 0;            // writer
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
        format::ArchiveReader archiveReader(input, maxFrameBytes_, memoryLimitBytes_);
        auto opened = archiveReader.open();
        if (!opened) {
            readerError = opened.error();
            stopSource.request_stop();
            queue1.close();
            return;
        }
        metadata = *opened;

        std::uint64_t frameId = 0;
        while (!stopToken.stop_requested()) {
            auto raw = archiveReader.readRawFrame();
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
            if (!queue1.push(std::move(in), stopToken)) {
                break;
            }
        }
        encodedBytes = archiveReader.stats().encodedBytes;
        queue1.close();
    });

    // N decoder workers: decodeRawFrame is pure computation. Workers do not
    // close queue2; the main thread closes it after join.
    auto decoderLoop = [&] {
        while (!stopToken.stop_requested()) {
            auto in = queue1.pop(stopToken);
            if (!in.has_value()) {
                break;
            }
            auto decoded = format::decodeRawFrame(*in->frame);
            if (!decoded) {
                {
                    std::lock_guard lk(decoderErrorMutex);
                    if (!decoderError) {
                        decoderError = decoded.error();
                    }
                }
                stopSource.request_stop();
                break;
            }
            OrderedFrame out{in->frameId,
                             std::make_unique<format::DecodedFrame>(std::move(*decoded))};
            if (!queue2.push(std::move(out), stopToken)) {
                break;
            }
        }
    };

    std::vector<std::jthread> decoders;
    decoders.reserve(parallelism_);
    for (std::size_t i = 0; i < parallelism_; ++i) {
        decoders.emplace_back(decoderLoop);
    }

    // Writer: reorder by frameId, then accumulate stats + rolling global
    // checksum (order-dependent -- must stay on this thread) and invoke the
    // sink. A sink failure cancels the pipeline.
    std::jthread writer([&] {
        ReorderBuffer<std::unique_ptr<format::DecodedFrame>> reorder;
        while (!stopToken.stop_requested()) {
            auto out = queue2.pop(stopToken);
            if (!out.has_value()) {
                break;
            }
            auto ready = reorder.submit(out->frameId, std::move(out->frame));
            for (auto& frame : ready) {
                stats.frameCount += 1;
                stats.recordCount += frame->recordCount;
                stats.totalBases += frame->totalBases;
                finalGlobalChecksum =
                    format::advanceGlobalChecksum(finalGlobalChecksum, frame->checksum);
                auto sinkResult = sink(std::move(frame->records));
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

    reader.join();
    for (auto& decoder : decoders) {
        decoder.join();
    }
    // All decoders have stopped producing; signal end-of-stream so the writer
    // drains queue2 and exits instead of blocking on an empty pop.
    queue2.close();
    writer.join();

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
    if (footerData->frameCount != stats.frameCount ||
        footerData->recordCount != stats.recordCount ||
        footerData->totalBases != stats.totalBases) {
        return makeError<DecompressStats>(ErrorCode::kFormatError, "FQC v2 footer totals disagree");
    }
    if (footerData->globalChecksum != finalGlobalChecksum) {
        return makeError<DecompressStats>(ErrorCode::kChecksumError,
                                          "FQC v2 global checksum mismatch");
    }

    stats.metadata = metadata;
    stats.encodedBytes = encodedBytes;
    return stats;
}

}  // namespace fqc::pipeline
