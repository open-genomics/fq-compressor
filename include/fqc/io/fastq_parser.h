// =============================================================================
// fq-compressor - FASTQ Parser
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace fqc::io {

/// Upper bound for a single FASTQ line. Real reads top out at a few megabases,
/// so the cap turns a malformed or hostile multi-gigabyte line into a clean
/// format error instead of an unbounded allocation. A line beyond the cap
/// still aborts the parse; in pipeline workers the abort surfaces through the
/// exception barrier as an internal error rather than std::terminate.
inline constexpr std::size_t kDefaultMaxFastqLineBytes = std::size_t{64} * 1024 * 1024;

/// Strip trailing `\r` left by CRLF inputs after `std::getline` consumes `\n`.
inline void trimTrailingCr(std::string& str) {
    // 只剥离与换行成对的单个 CRLF 行尾 \r；数据中真实存在的尾随 \r 不是行尾伪影，
    // 必须保留（否则 "SEQ\r\r\n" 会丢两个字节，破坏逐字节无损）。
    // 边界说明：LF-only 文件里字段内容恰以单个 \r 结尾时，该 \r 与 CRLF 行尾在
    // 字节流上不可区分，会被一并规范化——字节级无损契约仅对 LF-only 规范 FASTQ 成立。
    if (!str.empty() && str.back() == '\r') {
        str.pop_back();
    }
}

class FastqParser {
public:
    /// `maxLineBytes` bounds a single line before it is handed to validation;
    /// defaults to `kDefaultMaxFastqLineBytes`.
    explicit FastqParser(std::istream& stream,
                         std::size_t maxLineBytes = kDefaultMaxFastqLineBytes);

    [[nodiscard]] auto readRecord() -> Result<std::optional<ReadRecord>>;

    [[nodiscard]] auto lineNumber() const noexcept -> std::uint64_t {
        return lineNumber_;
    }

    [[nodiscard]] auto recordNumber() const noexcept -> std::uint64_t {
        return recordNumber_;
    }

    /// Raw bytes consumed so far, including line delimiters (and any `\r`
    /// later trimmed). Exact for plain uncompressed streams -- use it to
    /// resume parsing from the same byte offset. For gzip streams it counts
    /// *decompressed* bytes, which do not map to file offsets.
    [[nodiscard]] auto bytesConsumed() const noexcept -> std::uint64_t {
        return bytesConsumed_;
    }

private:
    [[nodiscard]] auto readLine(std::string& line) -> bool;
    /// Fail with `kIOError` on stream failure, `kFormatError` on unexpected EOF
    /// or on a line exceeding `maxLineBytes_`.
    [[nodiscard]] auto readRequiredLine(std::string& line) -> VoidResult;
    [[nodiscard]] auto formatError(std::string_view detail) const -> Error;
    /// Error when the underlying stream fails (e.g. corrupt gzip), as opposed
    /// to a clean EOF. The streambuf signals this via badbit.
    [[nodiscard]] static auto streamReadError() -> Error;

    std::istream& stream_;
    std::size_t maxLineBytes_;
    std::uint64_t lineNumber_ = 0;
    std::uint64_t recordNumber_ = 0;
    std::uint64_t bytesConsumed_ = 0;
    bool eof_ = false;
    bool streamError_ = false;
    /// Set when a read line exceeded `maxLineBytes_`; the next readRecord
    /// surfaces it as `kFormatError` instead of treating the line as EOF.
    bool lineTooLong_ = false;
};

/// One pair of reads; `second` is empty in single-end mode.
struct ReadPair {
    ReadRecord first;
    std::optional<ReadRecord> second;
};

/// Read the next record (or pair, when `mate` is non-null).
/// - success + value: a record; `second` is set in paired mode
/// - success + empty: clean EOF; both ends EOF in paired mode
/// - error: parse failure, or paired record counts disagree (`kFormatError`)
[[nodiscard]] auto readRecordPair(FastqParser& primary,
                                  FastqParser* mate) -> Result<std::optional<ReadPair>>;

}  // namespace fqc::io
