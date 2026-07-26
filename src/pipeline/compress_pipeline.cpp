// =============================================================================
// fq-compressor - Concurrent 3-Stage Compression Pipeline
// =============================================================================

#include "fqc/pipeline/compress_pipeline.h"

#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/spsc_queue.h"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
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

}  // namespace

CompressPipeline::CompressPipeline(std::size_t targetFrameBytes, bool paired)
    : targetFrameBytes_(targetFrameBytes), paired_(paired) {}

auto CompressPipeline::run(std::istream& primary,
                           std::istream* mate,
                           std::span<const ReadRecord> initialRecords,
                           format::ArchiveWriter& writer) -> Result<PipelineStats> {
    // Stage 1 -> 2: parsed FASTQ records accumulated into bounded frames.
    SpscQueue<std::vector<ReadRecord>, kDefaultQueueDepth> queue1;
    // Stage 2 -> 3: encoded (pre-compression) frames. unique_ptr so the queue
    // only moves the frame pointer, not the raw streams.
    SpscQueue<std::unique_ptr<format::EncodedFrame>, kDefaultQueueDepth> queue2;
    std::optional<Error> readerError;
    std::optional<Error> encoderError;
    std::optional<Error> writerError;
    PipelineStats stats;
    std::uint64_t logicalBytes = 0;

    // Cooperative cancellation. Any stage, on failure, calls request_stop();
    // the other stages' blocked push/pop wakes through the stop_token CV wait
    // and exits. Normal end-of-production chains close(): reader closes queue1,
    // encoder sees nullopt and closes queue2, compressor sees nullopt.
    std::stop_source stopSource;
    std::stop_token stopToken = stopSource.get_token();

    // Synchronization invariant: each shared variable is written by exactly one
    // worker -- `readerError`/`logicalBytes` by reader, `encoderError` by
    // encoder, `writerError`/`stats` (except logicalBytes) by compressor. The
    // workers share no mutable state but the two queues (SPSC, thread-safe).
    // The main thread reads these only after the matching join() below, whose
    // happens-before edge makes the access safe without extra synchronization.
    std::jthread reader([&] {
        io::FastqParser parser(primary);
        std::optional<io::FastqParser> mateParser;
        if (mate != nullptr) {
            mateParser.emplace(*mate);
        }

        std::vector<ReadRecord> frame;
        std::size_t retainedBytes = 0;

        auto frameFull = [&] {
            return retainedBytes >= targetFrameBytes_ && (!paired_ || frame.size() % 2 == 0);
        };

        auto pushFrame = [&] -> bool {
            if (frame.empty()) {
                return true;
            }
            bool ok = queue1.push(std::move(frame), stopToken);
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
            auto pair = io::readRecordPair(parser, mateParser ? &*mateParser : nullptr);
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
    });

    // Stage 2: pop ReadRecord frames, encode (CPU-only) into EncodedFrame,
    // push to queue2. encodeFrame is a free function -- no writer state touched.
    std::jthread encoder([&] {
        while (!stopToken.stop_requested()) {
            auto frame = queue1.pop(stopToken);
            if (!frame.has_value()) {
                break;
            }
            auto encoded = format::encodeFrame(*frame, writer.options());
            if (!encoded) {
                encoderError = encoded.error();
                stopSource.request_stop();
                break;
            }
            if (!queue2.push(std::move(*encoded), stopToken)) {
                break;
            }
        }
        queue2.close();
    });

    // Stage 3: pop EncodedFrame, compress (zstd) + write to disk + update
    // stats. ArchiveWriter is owned by this thread (output_/stats_/checksum).
    std::jthread compressor([&] {
        while (!stopToken.stop_requested()) {
            auto encoded = queue2.pop(stopToken);
            if (!encoded.has_value()) {
                break;
            }
            stats.recordCount += (*encoded)->recordCount;
            stats.frameCount += 1;
            auto result = writer.writeEncodedFrame(std::move(*encoded));
            if (!result) {
                writerError = result.error();
                stopSource.request_stop();
                break;
            }
        }
    });

    reader.join();
    encoder.join();
    compressor.join();

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
