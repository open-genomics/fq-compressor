// =============================================================================
// fq-compressor - FASTQ Parser
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace fqc::io {

/// Strip trailing `\r` left by CRLF inputs after `std::getline` consumes `\n`.
inline void trimTrailingCr(std::string& str) {
    while (!str.empty() && str.back() == '\r') {
        str.pop_back();
    }
}

class FastqParser {
public:
    explicit FastqParser(std::istream& stream);

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
    /// Fail with `kIOError` on stream failure, `kFormatError` on unexpected EOF.
    [[nodiscard]] auto readRequiredLine(std::string& line) -> VoidResult;
    [[nodiscard]] auto formatError(std::string_view detail) const -> Error;
    /// Error when the underlying stream fails (e.g. corrupt gzip), as opposed
    /// to a clean EOF. The streambuf signals this via badbit.
    [[nodiscard]] static auto streamReadError() -> Error;

    std::istream& stream_;
    std::uint64_t lineNumber_ = 0;
    std::uint64_t recordNumber_ = 0;
    std::uint64_t bytesConsumed_ = 0;
    bool eof_ = false;
    bool streamError_ = false;
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
[[nodiscard]] auto readRecordPair(FastqParser& primary, FastqParser* mate)
    -> Result<std::optional<ReadPair>>;

}  // namespace fqc::io
