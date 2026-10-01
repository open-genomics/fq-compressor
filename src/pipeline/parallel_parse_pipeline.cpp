// =============================================================================
// fq-compressor - Parallel-Parse Compression Pipeline
// =============================================================================

#include "fqc/pipeline/parallel_parse_pipeline.h"

#include "fqc/common/types.h"
#include "fqc/format/archive.h"
#include "fqc/io/fastq_parser.h"
#include "fqc/pipeline/chunk_orderer.h"
#include "fqc/pipeline/frame_accumulator.h"
#include "fqc/pipeline/in_flight_limiter.h"
#include "fqc/pipeline/mpmc_queue.h"
#include "fqc/pipeline/timing.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fqc::pipeline {

namespace {

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

struct ReadyFrame {
    std::uint64_t chunk = 0;
    std::unique_ptr<format::CompressedFrame> frame;
};

/// Reads one raw line, advancing the absolute `offset` past the delimiter.
/// Trims '\r' like the parser does. Returns false at EOF.
[[nodiscard]] auto readRawLine(std::istream& input,
                               std::string& line,
                               std::uint64_t& offset) -> bool {
    if (!std::getline(input, line)) {
        return false;
    }
    offset += line.size() + (input.eof() ? 0 : 1);
    io::trimTrailingCr(line);
    return true;
}

/// 流水线 worker 的异常屏障消息。jthread 函数体逃逸的异常会直接 std::terminate
/// （main 的 catch-all 看不到线程内异常），这里统一转成 kInternalError 记录，
/// 由调用方配合 request_stop() 完成协作取消。
[[nodiscard]] auto workerCrashError(std::string_view stage, const char* detail) -> Error {
    return Error{ErrorCode::kInternalError, std::string(stage) + " worker terminated: " + detail};
}

}  // namespace

/// 对齐窗口：块边界至多落在下一记录起点前 3 个完整行（跨界记录剩余的
/// sequence/'+'/quality 行，由上一 worker 解析并校验），校验起点记录又需要它的
/// 完整 4 行，因此 7 个非空行必然容纳合法输入的第一条记录起点。
constexpr std::size_t kAlignmentWindowLines = 7;

/// 镜像 FastqParser 的结构验收：'@' 头、'+' 行、sequence 与 quality 等长。
/// 内容校验（IUPAC 字符集等）刻意留给 parser/encodeFrame——这里拒绝内容
/// 畸形会静默丢掉顺序路径会大声报错的记录。
[[nodiscard]] auto isStructuralRecord(const std::string& header,
                                      const std::string& sequence,
                                      const std::string& plusLine,
                                      const std::string& quality) -> bool {
    return !header.empty() && header[0] == '@' && !plusLine.empty() && plusLine[0] == '+' &&
        sequence.size() == quality.size();
}

/// 残段形状可行性：记录起点前的 `count` 个非空行必须可能是一个合法记录的
/// 行后缀（该记录由上一 worker 解析），否则这个 framing 在合法输入中不可达。
/// 可行形状：[]、[qual]、['+', qual]、[seq, '+', qual]。qual 行内容任意，
/// '+' 行是唯一的形状锚点——没有锚点却要求更长的后缀，说明数据已损坏
/// （如缺 '+' 行的记录），必须交给上层报错而不是继续跳过。
auto findFirstRecordStart(std::istream& input,
                          std::uint64_t baseOffset,
                          std::size_t maxLineBytes) -> Result<std::optional<std::uint64_t>> {
    std::uint64_t offset = baseOffset;

    // 第一步：行首对齐。baseOffset 落在行中段时 getline 读出的是行尾残段——它属于
    // 上一 worker 正在解析的跨界记录，这里只消费、不当候选；随后重读的下一行是
    // 完整行，正常进窗口。记录被丢弃残段的首字符：它是 '+' 时，窗口第一行可能是
    // 该记录的 '@' 质量行（假警报，可跳过）。
    std::string first;
    std::uint64_t firstStart = baseOffset;
    std::optional<char> partialFirstChar;
    if (!readRawLine(input, first, offset)) {
        return std::optional<std::uint64_t>{};
    }
    if (baseOffset != 0) {
        input.clear();
        input.seekg(static_cast<std::streamoff>(baseOffset - 1));
        const bool atLineBoundary = input.peek() == '\n';
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset));
        if (!atLineBoundary) {
            partialFirstChar =
                first.empty() ? std::optional<char>{} : std::optional<char>(first[0]);
            firstStart = offset;
            if (!readRawLine(input, first, offset)) {
                return std::optional<std::uint64_t>{};
            }
        }
    }

    // 第二步：收集窗口。记录内部不会出现空行（顺序路径对空 sequence/'+' 行报错），
    // 记录之间的空行与顺序路径一样跳过。窗口 7 行 = 至多 3 残段行 + 4 行起点记录。
    std::array<std::string, kAlignmentWindowLines> lines;
    std::array<std::uint64_t, kAlignmentWindowLines> starts{};
    std::array<bool, kAlignmentWindowLines> skipped{};
    std::size_t count = 0;
    bool hitEof = false;
    if (!first.empty()) {
        if (first.size() > maxLineBytes) {
            return makeError<std::optional<std::uint64_t>>(
                ErrorCode::kFormatError,
                "FASTQ line exceeds maximum length near byte offset " + std::to_string(firstStart));
        }
        lines[0] = std::move(first);
        starts[0] = firstStart;
        count = 1;
    }
    while (count < kAlignmentWindowLines) {
        const std::uint64_t start = offset;
        std::string next;
        if (!readRawLine(input, next, offset)) {
            hitEof = true;
            break;
        }
        if (next.empty()) {
            continue;
        }
        if (next.size() > maxLineBytes) {
            return makeError<std::optional<std::uint64_t>>(
                ErrorCode::kFormatError,
                "FASTQ line exceeds maximum length near byte offset " + std::to_string(start));
        }
        lines[count] = std::move(next);
        starts[count] = start;
        ++count;
    }

    // 残段形状可行性：候选前的非跳过行必须可能是一个合法记录的行后缀（该记录由
    // 上一 worker 解析），否则这个 framing 在合法输入中不可达。可行形状：[]、
    // [qual]、['+', qual]、[seq, '+', qual]；'+' 行是唯一的形状锚点。质量行假警报
    // 被跳过（skipped）后不计入残段。行序按物理顺序（最早在前）。
    auto residueFeasible = [&](std::size_t candidate) -> bool {
        std::array<const std::string*, 3> r{};
        std::size_t n = 0;
        for (std::size_t i = 0; i < candidate && n < 3; ++i) {
            if (skipped[i]) {
                continue;
            }
            r[n++] = &lines[i];
        }
        switch (n) {
            case 0:
            case 1:
                return true;
            case 2:
                return !r[0]->empty() && (*r[0])[0] == '+';
            case 3:
                return !r[1]->empty() && (*r[1])[0] == '+';
            default:
                return false;
        }
    };

    // 质量行判定：结构拒绝的 '@' 候选，仅当其前一行是 '+'（p≥1）或残段首字符是
    // '+'（p==0，残段是 '+' 行的后半）时，才可能是上一条记录的 '@' 质量行
    // （Phred+33 Q31）假警报而应跳过；否则它是畸形记录头（缺 '+' 行、长度不匹配、
    // 只有 id 行等），顺序路径会在同一位置报错，这里必须返回它让 worker 的 parser
    // 同样报错，否则该记录被静默丢弃。
    auto isQualityLine = [&](std::size_t p) -> bool {
        if (p == 0) {
            return partialFirstChar.has_value() && *partialFirstChar == '+';
        }
        for (std::size_t i = p; i > 0; --i) {
            if (!skipped[i - 1]) {
                return !lines[i - 1].empty() && lines[i - 1][0] == '+';
            }
        }
        return false;
    };

    // 第三步：顺序扫描所有候选。命中结构合法且残段可行的返回；结构拒绝的按质量行
    // 判定分流——假警报跳过，畸形头返回（让 parser 报错）。
    for (std::size_t p = 0; p < count; ++p) {
        if (skipped[p] || lines[p].empty() || lines[p][0] != '@') {
            continue;
        }
        if (p + 3 < count) {
            if (isStructuralRecord(lines[p], lines[p + 1], lines[p + 2], lines[p + 3])) {
                if (residueFeasible(p)) {
                    return starts[p];
                }
                // 结构合法但残段不可行（如缺 '+' 记录的伪候选）：继续扫，窗口收满
                // 仍未命中时报错。
            } else if (isQualityLine(p)) {
                skipped[p] = true;  // 质量行假警报：跳过，后续候选的残段排除该行
            } else {
                return starts[p];  // 畸形头：worker 的 parser 报与顺序路径相同的错
            }
        } else if (hitEof && p + 1 < count) {
            // EOF 尾部、带后继行的 '@' 候选：截断记录（或最后的质量行 + 残缺头），
            // parser 报与顺序路径一致的错，不静默丢弃。
            return starts[p];
        }
    }

    // 无候选命中。窗口收满（未到 EOF）意味着 7 行内没有任何可行 framing，对齐区
    // 里有顺序路径同样拒绝的损坏数据。
    if (!hitEof) {
        return makeError<std::optional<std::uint64_t>>(
            ErrorCode::kFormatError,
            "no valid FASTQ record starts within the alignment window at byte offset " +
                std::to_string(baseOffset));
    }

    // EOF 收尾。剩余非跳过行若仍可能是上一记录的行后缀（≤3 行且形状可行），
    // 保守返回空（如文件末尾的 '@' 质量行）；否则对齐区含损坏数据，报错。
    if (!residueFeasible(count)) {
        return makeError<std::optional<std::uint64_t>>(
            ErrorCode::kFormatError,
            "corrupt FASTQ tail before EOF at byte offset " + std::to_string(baseOffset));
    }
    return std::optional<std::uint64_t>{};
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
      parserParallelism_(parallelism == 0 ? 1 : parallelism),
      encoderParallelism_(std::min(parserParallelism_, kDefaultEncoderParallelism)) {}

// 并行解析管线按帧号维护一个有序窗口（chunk 乱序产出、严格按序提交），
// 每个阶段的状态机交错是这一不变量所必需，拆分为独立函数反而更难推理。
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto ParallelParsePipeline::run(std::span<const ReadRecord> initialRecords,
                                format::ArchiveWriter& writer) -> Result<PipelineStats> {
    MpmcQueue<ParseItem, kDefaultQueueDepth> queue1;
    MpmcQueue<OrderedItem, kDefaultQueueDepth> queue2;
    const auto inFlightLimit = MpmcQueue<ParseItem, kDefaultQueueDepth>::kUsableCapacity +
        MpmcQueue<OrderedItem, kDefaultQueueDepth>::kUsableCapacity + parserParallelism_;
    InFlightLimiter frameLimiter(inFlightLimit, parserParallelism_);
    std::optional<Error> parseError;
    std::optional<Error> encoderError;
    std::optional<Error> writerError;
    std::mutex errorMutex;
    PipelineStats stats;
    stats.parserWorkers = parserParallelism_;
    stats.encoderWorkers = encoderParallelism_;
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
        const std::lock_guard lk(errorMutex);
        if (!parseError) {
            parseError = error;
        }
    };

    // K parser workers, one per byte chunk of [sampleEnd, fileSize).
    // Worker 0 additionally seeds its accumulator with the profile sample, so
    // framing is continuous across the sample boundary (and identical to the
    // sequential pipeline when K == 1). Every worker always emits exactly one
    // chunk-completion marker -- the writer's ChunkOrderer stalls without it.
    const std::uint64_t region = fileSize_ > sampleEndOffset_ ? fileSize_ - sampleEndOffset_ : 0;
    const std::uint64_t step =
        region == 0 ? 0 : (region + parserParallelism_ - 1) / parserParallelism_;

    auto parseWorker = [&](std::uint64_t workerIndex) {
        std::uint64_t parseNs = 0;
        std::uint64_t pushNs = 0;
        std::uint64_t localLogical = 0;
        std::uint64_t localId = 0;
        // marker 推送必须在异常屏障之外可达：catch 之后的无条件补发依赖它。
        auto pushMarker = [&](ParseItem item) -> bool {
            const auto pushStart = Clock::now();
            const bool ok = queue1.push(std::move(item), stopToken);
            pushNs += nanosSince(pushStart);
            return ok;
        };
        try {
            FrameAccumulator accumulator(targetFrameBytes_, false);

            auto pushFrame = [&](ParseItem item) -> bool {
                if (!frameLimiter.acquire(workerIndex, stopToken)) {
                    return false;
                }
                const auto pushStart = Clock::now();
                const bool ok = queue1.push(std::move(item), stopToken);
                pushNs += nanosSince(pushStart);
                if (!ok) {
                    frameLimiter.release(workerIndex);
                }
                return ok;
            };

            if (workerIndex == 0) {
                for (const auto& record : initialRecords) {
                    localLogical += canonicalFastqBytes(record);
                    if (auto closed = accumulator.append(ReadRecord(record))) {
                        if (!pushFrame(ParseItem{0, localId++, false, std::move(*closed)})) {
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
                (workerIndex + 1 == parserParallelism_) ? fileSize_ : chunkBegin + step;

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
                        recordError(Error{ErrorCode::kIOError,
                                          "failed to seek input for parallel parsing"});
                        stopSource.request_stop();
                    } else {
                        auto found = findFirstRecordStart(file, chunkBegin);
                        if (!found) {
                            recordError(found.error());
                            stopSource.request_stop();
                        } else {
                            recordStart = found->value_or(fileSize_);
                        }
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
                        recordError(Error{ErrorCode::kIOError,
                                          "failed to seek input for parallel parsing"});
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
                        if (!pushFrame(
                                ParseItem{workerIndex, localId++, false, std::move(*closed)})) {
                            break;
                        }
                    }
                }
            }

            if (!stopToken.stop_requested()) {
                if (auto tail = accumulator.finish()) {
                    pushFrame(ParseItem{workerIndex, localId++, false, std::move(*tail)});
                }
            }
        } catch (const std::exception& error) {
            recordError(workerCrashError("parallel parser", error.what()));
            stopSource.request_stop();
        } catch (...) {
            recordError(workerCrashError("parallel parser", "unknown exception"));
            stopSource.request_stop();
        }
        // 完整性协议：每个 worker 无条件恰好发一个 chunk-end marker（异常路径
        // 也一样），否则 writer 的 ChunkOrderer 会永远等不到该 chunk 的完成标记。
        pushMarker(ParseItem{workerIndex, localId, true, {}});

        logicalBytes.fetch_add(localLogical, std::memory_order_relaxed);
        readerParseNs.fetch_add(parseNs, std::memory_order_relaxed);
        readerPushNs.fetch_add(pushNs, std::memory_order_relaxed);
    };

    std::vector<std::jthread> parsers;
    parsers.reserve(parserParallelism_);
    for (std::uint64_t i = 0; i < parserParallelism_; ++i) {
        parsers.emplace_back(parseWorker, i);
    }

    // N encoder workers (same as CompressPipeline, plus marker pass-through).
    auto encoderLoop = [&] {
        std::uint64_t popNs = 0;
        std::uint64_t encodeNs = 0;
        std::uint64_t compressNs = 0;
        std::uint64_t pushNs = 0;
        try {
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
                        const std::lock_guard lk(errorMutex);
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
                        const std::lock_guard lk(errorMutex);
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
        } catch (const std::exception& error) {
            const std::lock_guard lk(errorMutex);
            if (!encoderError) {
                encoderError = workerCrashError("parallel encoder", error.what());
            }
            stopSource.request_stop();
        } catch (...) {
            const std::lock_guard lk(errorMutex);
            if (!encoderError) {
                encoderError = workerCrashError("parallel encoder", "unknown exception");
            }
            stopSource.request_stop();
        }
        encoderPopNs.fetch_add(popNs, std::memory_order_relaxed);
        encoderEncodeNs.fetch_add(encodeNs, std::memory_order_relaxed);
        encoderCompressNs.fetch_add(compressNs, std::memory_order_relaxed);
        encoderPushNs.fetch_add(pushNs, std::memory_order_relaxed);
    };

    std::vector<std::jthread> encoders;
    encoders.reserve(encoderParallelism_);
    for (std::size_t i = 0; i < encoderParallelism_; ++i) {
        encoders.emplace_back(encoderLoop);
    }

    // Writer: drain in (chunk, local) lexicographic order via ChunkOrderer.
    std::jthread writerThread([&] {
        std::uint64_t popNs = 0;
        std::uint64_t writeNs = 0;
        try {
            ChunkOrderer<ReadyFrame> orderer;
            auto emitReady = [&](std::vector<ReadyFrame> ready) {
                for (auto& readyFrame : ready) {
                    stats.recordCount += readyFrame.frame->recordCount;
                    stats.frameCount += 1;
                    const auto writeStart = Clock::now();
                    auto result = writer.writeCompressedFrame(std::move(readyFrame.frame));
                    writeNs += nanosSince(writeStart);
                    frameLimiter.release(readyFrame.chunk);
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
                    emitReady(orderer.submitFrame(
                        out->chunk, out->local, ReadyFrame{out->chunk, std::move(out->frame)}));
                }
                if (writerError.has_value()) {
                    break;
                }
            }
        } catch (const std::exception& error) {
            writerError = workerCrashError("parallel writer", error.what());
            stopSource.request_stop();
        } catch (...) {
            writerError = workerCrashError("parallel writer", "unknown exception");
            stopSource.request_stop();
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
    stats.inFlightHighWater = frameLimiter.highWater();

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
