// =============================================================================
// fq-compressor - Concurrent 2-Stage Compression Pipeline
// =============================================================================

#include "fqc/pipeline/compress_pipeline.h"

#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/spsc_queue.h"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <span>
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
    SpscQueue<std::vector<ReadRecord>, kDefaultQueueDepth> queue;
    std::optional<Error> readerError;
    std::optional<Error> writerError;
    PipelineStats stats;
    std::uint64_t logicalBytes = 0;

    // Synchronization invariant: `readerError`/`logicalBytes` are written only
    // by the reader thread, `writerError`/`stats` (except logicalBytes) only by
    // the writer thread. The two workers share no mutable state but `queue`
    // (SPSC, thread-safe by construction). Each worker's writes are read by the
    // main thread only after the matching join() below, whose happens-before
    // edge makes the access safe without extra synchronization -- do not read
    // them earlier, and do not let the other thread touch them.
    std::thread reader([&] {
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
            bool ok = queue.push(std::move(frame));
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
            if (queue.isAborted()) {
                break;
            }
            if (!append(std::move(record))) {
                break;
            }
        }

        while (!queue.isAborted()) {
            auto pair = io::readRecordPair(parser, mateParser ? &*mateParser : nullptr);
            if (!pair) {
                readerError = pair.error();
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

        if (!queue.isAborted()) {
            pushFrame();
        }
        queue.close();
    });

    std::thread writerThread([&] {
        while (true) {
            auto frame = queue.pop();
            if (!frame.has_value()) {
                break;
            }
            stats.recordCount += frame->size();
            stats.frameCount += 1;
            auto result = writer.writeFrame(*frame);
            if (!result) {
                writerError = result.error();
                queue.abort();
                break;
            }
        }
    });

    reader.join();
    writerThread.join();

    stats.logicalBytes = logicalBytes;

    if (writerError.has_value()) {
        return makeError<PipelineStats>(std::move(*writerError));
    }
    if (readerError.has_value()) {
        return makeError<PipelineStats>(std::move(*readerError));
    }
    return stats;
}

}  // namespace fqc::pipeline
