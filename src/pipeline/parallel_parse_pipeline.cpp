// =============================================================================
// fq-compressor - Parallel-Parse Compression Pipeline (Stage H)
// =============================================================================

#include "fqc/pipeline/parallel_parse_pipeline.h"

#include "fqc/common/types.h"
#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/chunk_orderer.h"
#include "fqc/pipeline/frame_accumulator.h"
#include "fqc/pipeline/mpmc_queue.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fqc::pipeline {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] auto nanosSince(Clock::time_point start) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

/// One queue1 item: either a parsed frame or a chunk-completion marker
/// (`chunkEnd`, with `local` = the chunk's total frame count). The writer's
/// ChunkOrderer needs a marker from EVERY chunk, including zero-frame ones.
struct ParseItem {
    std::uint64_t chunk = 0;
    std::uint64_t local = 0;
    bool chunkEnd = false;
    std::vector<ReadRecord> records;
};

struct OrderedItem {
    std::uint64_t chunk = 0;
    std::uint64_t local = 0;
    bool chunkEnd = false;
    std::unique_ptr<format::CompressedFrame> frame;
};

void trimRight(std::string& str) {
    while (!str.empty() && str.back() == '\r') {
        str.pop_back();
    }
}

/// Reads one raw line, advancing the absolute `offset` past the delimiter.
/// Trims '\r' like the parser does. Returns false at EOF.
[[nodiscard]] auto readRawLine(std::istream& input,
                               std::string& line,
                               std::uint64_t& offset) -> bool {
    if (!std::getline(input, line)) {
        return false;
    }
    offset += line.size() + (input.eof() ? 0 : 1);
    trimRight(line);
    return true;
}

}  // namespace

auto findFirstRecordStart(std::istream& input,
                          std::uint64_t baseOffset) -> std::optional<std::uint64_t> {
    std::string l0;
    std::string l1;
    std::string l2;
    std::string l3;
    std::uint64_t offset = baseOffset;
    while (true) {
        const std::uint64_t lineStart = offset;
        if (!readRawLine(input, l0, offset)) {
            return std::nullopt;
        }
        if (l0.empty() || l0[0] != '@') {
            continue;
        }
        // Candidate record start. The check mirrors FastqParser's STRUCTURAL
        // acceptance only ('+' line, equal lengths) and deliberately not
        // encodeFrame's content validation: rejecting a candidate on content
        // here would silently skip a record that the sequential path parses
        // and then rejects loudly. A '@' quality line still fails because a
        // well-formed sequence line never starts with '+' (stage H trap 1).
        const std::uint64_t afterL0 = offset;
        const bool haveL1 = readRawLine(input, l1, offset);
        const bool have4 =
            haveL1 && readRawLine(input, l2, offset) && readRawLine(input, l3, offset);
        if (!have4) {
            // EOF during the structural check. With nothing (or one blank
            // line) after the '@' line this is indistinguishable from the
            // file's final '@'-starting quality line, which the previous
            // chunk already parsed -- stay conservative. Any other shape is
            // a record truncated mid-body: it starts in this chunk, so hand
            // it to the parser, which raises the same error the sequential
            // path would, instead of dropping the record silently.
            if (!haveL1 || l1.empty()) {
                return std::nullopt;
            }
            return lineStart;
        }
        if (!l2.empty() && l2[0] == '+' && l1.size() == l3.size()) {
            return lineStart;
        }
        // False alarm: rewind to right after l0 and resume scanning there
        // (l1 itself may be the real header and must be re-examined).
        input.clear();
        input.seekg(static_cast<std::streamoff>(afterL0));
        if (!input) {
            return std::nullopt;
        }
        offset = afterL0;
    }
}

ParallelParsePipeline::ParallelParsePipeline(std::filesystem::path inputPath,
                                             std::uint64_t fileSize,
                                             std::size_t targetFrameBytes,
                                             std::uint64_t sampleEndOffset,
                                             std::size_t parallelism)
    : inputPath_(std::move(inputPath)),
      fileSize_(fileSize),
      targetFrameBytes_(targetFrameBytes),
      sampleEndOffset_(sampleEndOffset),
      parallelism_(parallelism == 0 ? 1 : parallelism) {}

auto ParallelParsePipeline::run(std::span<const ReadRecord> initialRecords,
                                format::ArchiveWriter& writer) -> Result<PipelineStats> {
    MpmcQueue<ParseItem, kDefaultQueueDepth> queue1;
    MpmcQueue<OrderedItem, kDefaultQueueDepth> queue2;
    std::optional<Error> parseError;
    std::optional<Error> encoderError;
    std::optional<Error> writerError;
    std::mutex errorMutex;
    PipelineStats stats;
    std::atomic<std::uint64_t> logicalBytes{0};

    const auto wallStart = Clock::now();
    std::atomic<std::uint64_t> readerParseNs{0};
    std::atomic<std::uint64_t> readerPushNs{0};
    std::atomic<std::uint64_t> encoderPopNs{0};
    std::atomic<std::uint64_t> encoderEncodeNs{0};
    std::atomic<std::uint64_t> encoderCompressNs{0};
    std::atomic<std::uint64_t> encoderPushNs{0};
    std::atomic<std::uint64_t> writerPopNs{0};
    std::atomic<std::uint64_t> writerWriteNs{0};

    std::stop_source stopSource;
    std::stop_token stopToken = stopSource.get_token();

    auto recordError = [&](const Error& error) {
        std::lock_guard lk(errorMutex);
        if (!parseError) {
            parseError = error;
        }
    };

    // Stage 1: K parser workers, one per byte chunk of [sampleEnd, fileSize).
    // Worker 0 additionally seeds its accumulator with the profile sample, so
    // framing is continuous across the sample boundary (and identical to the
    // sequential pipeline when K == 1). Every worker always emits exactly one
    // chunk-completion marker -- the writer's ChunkOrderer stalls without it.
    const std::uint64_t region = fileSize_ > sampleEndOffset_ ? fileSize_ - sampleEndOffset_ : 0;
    const std::uint64_t step = region == 0 ? 0 : (region + parallelism_ - 1) / parallelism_;

    auto parseWorker = [&](std::uint64_t workerIndex) {
        std::uint64_t parseNs = 0;
        std::uint64_t pushNs = 0;
        std::uint64_t localLogical = 0;
        std::uint64_t localId = 0;
        FrameAccumulator accumulator(targetFrameBytes_, false);

        auto pushItem = [&](ParseItem item) -> bool {
            const auto pushStart = Clock::now();
            const bool ok = queue1.push(std::move(item), stopToken);
            pushNs += nanosSince(pushStart);
            return ok;
        };

        if (workerIndex == 0) {
            for (const auto& record : initialRecords) {
                localLogical += canonicalFastqBytes(record);
                if (auto closed = accumulator.append(ReadRecord(record))) {
                    if (!pushItem(ParseItem{0, localId++, false, std::move(*closed)})) {
                        break;
                    }
                }
                if (stopToken.stop_requested()) {
                    break;
                }
            }
        }

        const std::uint64_t chunkBegin = sampleEndOffset_ + workerIndex * step;
        const std::uint64_t chunkEnd =
            (workerIndex + 1 == parallelism_) ? fileSize_ : chunkBegin + step;

        // One stream per worker, shared by boundary alignment and parsing.
        std::ifstream file;
        std::uint64_t recordStart = chunkBegin;
        if (chunkBegin >= fileSize_) {
            recordStart = fileSize_;  // empty chunk: nothing starts here
        } else if (workerIndex != 0) {
            // Chunks after the first must boundary-align; worker 0 starts
            // exactly at the sample end, which is already a record boundary
            // (sampling consumed whole records on the main thread).
            file.open(inputPath_, std::ios::binary);
            if (!file) {
                recordError(
                    Error{ErrorCode::kIOError, "failed to open input for parallel parsing"});
                stopSource.request_stop();
            } else {
                file.seekg(static_cast<std::streamoff>(chunkBegin));
                if (!file) {
                    recordError(
                        Error{ErrorCode::kIOError, "failed to seek input for parallel parsing"});
                    stopSource.request_stop();
                } else {
                    auto found = findFirstRecordStart(file, chunkBegin);
                    recordStart = found.value_or(fileSize_);
                }
            }
        }

        if (recordStart < chunkEnd && !stopToken.stop_requested()) {
            if (file.is_open()) {
                file.clear();  // findFirstRecordStart may have left eof behind
            } else {
                file.open(inputPath_, std::ios::binary);
            }
            if (!file) {
                recordError(
                    Error{ErrorCode::kIOError, "failed to open input for parallel parsing"});
                stopSource.request_stop();
            } else {
                file.seekg(static_cast<std::streamoff>(recordStart));
                if (!file) {
                    recordError(
                        Error{ErrorCode::kIOError, "failed to seek input for parallel parsing"});
                    stopSource.request_stop();
                }
            }
            const std::uint64_t parseBase = recordStart;
            io::FastqParser parser(file);
            // Parse every record that STARTS inside [chunkBegin, chunkEnd);
            // a record straddling the boundary belongs to this worker,
            // the next worker's alignment skips it.
            while (file && recordStart < chunkEnd && !stopToken.stop_requested()) {
                const auto parseStart = Clock::now();
                auto record = parser.readRecord();
                parseNs += nanosSince(parseStart);
                if (!record) {
                    recordError(record.error());
                    stopSource.request_stop();
                    break;
                }
                if (!record->has_value()) {
                    break;  // EOF (last chunk)
                }
                recordStart = parseBase + parser.bytesConsumed();
                localLogical += canonicalFastqBytes(**record);
                if (auto closed = accumulator.append(std::move(**record))) {
                    if (!pushItem(ParseItem{workerIndex, localId++, false, std::move(*closed)})) {
                        break;
                    }
                }
            }
        }

        if (!stopToken.stop_requested()) {
            if (auto tail = accumulator.finish()) {
                pushItem(ParseItem{workerIndex, localId++, false, std::move(*tail)});
            }
        }
        // The marker is the ordering protocol's completeness guarantee: push
        // it even when cancelled (a rejected push under stop is harmless).
        pushItem(ParseItem{workerIndex, localId, true, {}});

        logicalBytes.fetch_add(localLogical, std::memory_order_relaxed);
        readerParseNs.fetch_add(parseNs, std::memory_order_relaxed);
        readerPushNs.fetch_add(pushNs, std::memory_order_relaxed);
    };

    std::vector<std::jthread> parsers;
    parsers.reserve(parallelism_);
    for (std::uint64_t i = 0; i < parallelism_; ++i) {
        parsers.emplace_back(parseWorker, i);
    }

    // Stage 2: N encoder workers (identical to CompressPipeline, plus marker
    // pass-through). Frames: encode + compress. Markers: forward untouched.
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
            if (in->chunkEnd) {
                OrderedItem marker{in->chunk, in->local, true, nullptr};
                const auto pushStart = Clock::now();
                const bool pushed = queue2.push(std::move(marker), stopToken);
                pushNs += nanosSince(pushStart);
                if (!pushed) {
                    break;
                }
                continue;
            }
            const auto encodeStart = Clock::now();
            auto encoded = format::encodeFrame(in->records, writer.options());
            encodeNs += nanosSince(encodeStart);
            if (!encoded) {
                {
                    std::lock_guard lk(errorMutex);
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
                    std::lock_guard lk(errorMutex);
                    if (!encoderError) {
                        encoderError = compressed.error();
                    }
                }
                stopSource.request_stop();
                break;
            }
            OrderedItem out{in->chunk, in->local, false, std::move(*compressed)};
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

    // Stage 3: pop ordered items, drain in (chunk, local) lexicographic order
    // via ChunkOrderer, write frames. On-disk frame id comes from the
    // writer's own counter, so lexicographic submission preserves the
    // archive's monotonic-frame-id invariant.
    std::jthread writerThread([&] {
        std::uint64_t popNs = 0;
        std::uint64_t writeNs = 0;
        ChunkOrderer<std::unique_ptr<format::CompressedFrame>> orderer;
        auto emitReady = [&](std::vector<std::unique_ptr<format::CompressedFrame>> ready) {
            for (auto& frame : ready) {
                stats.recordCount += frame->recordCount;
                stats.frameCount += 1;
                const auto writeStart = Clock::now();
                auto result = writer.writeCompressedFrame(std::move(frame));
                writeNs += nanosSince(writeStart);
                if (!result) {
                    writerError = result.error();
                    stopSource.request_stop();
                    return;
                }
            }
        };
        while (!stopToken.stop_requested()) {
            const auto popStart = Clock::now();
            auto out = queue2.pop(stopToken);
            popNs += nanosSince(popStart);
            if (!out.has_value()) {
                break;
            }
            if (out->chunkEnd) {
                emitReady(orderer.submitChunkEnd(out->chunk, out->local));
            } else {
                emitReady(orderer.submitFrame(out->chunk, out->local, std::move(out->frame)));
            }
            if (writerError.has_value()) {
                break;
            }
        }
        writerPopNs.store(popNs, std::memory_order_relaxed);
        writerWriteNs.store(writeNs, std::memory_order_relaxed);
    });

    for (auto& parser : parsers) {
        parser.join();
    }
    // All parser workers (and their markers) are in; signal end-of-stream so
    // encoders drain queue1 and exit.
    queue1.close();
    for (auto& encoder : encoders) {
        encoder.join();
    }
    queue2.close();
    writerThread.join();

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
    stats.logicalBytes = logicalBytes.load(std::memory_order_relaxed);

    if (writerError.has_value()) {
        return makeError<PipelineStats>(std::move(*writerError));
    }
    if (encoderError.has_value()) {
        return makeError<PipelineStats>(std::move(*encoderError));
    }
    if (parseError.has_value()) {
        return makeError<PipelineStats>(std::move(*parseError));
    }
    return stats;
}

}  // namespace fqc::pipeline
