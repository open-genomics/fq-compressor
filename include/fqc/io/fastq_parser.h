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

using FastqRecord = ReadRecord;

class FastqParser {
public:
    explicit FastqParser(std::istream& stream);

    [[nodiscard]] auto readRecord() -> Result<std::optional<FastqRecord>>;

    [[nodiscard]] auto lineNumber() const noexcept -> std::uint64_t {
        return lineNumber_;
    }

    [[nodiscard]] auto recordNumber() const noexcept -> std::uint64_t {
        return recordNumber_;
    }

    /// Raw bytes consumed so far, including line delimiters (and any `\r`
    /// later trimmed). Exact for plain uncompressed streams -- use it to
    /// resume parsing from the same byte offset (stage H parallel parsing).
    /// For gzip streams it counts *decompressed* bytes, which do not map to
    /// file offsets.
    [[nodiscard]] auto bytesConsumed() const noexcept -> std::uint64_t {
        return bytesConsumed_;
    }

private:
    [[nodiscard]] auto readLine(std::string& line) -> bool;
    static void trimRight(std::string& str);
    /// @brief Error returned when the underlying stream fails (e.g. corrupt gzip input),
    /// as opposed to a clean end-of-file. The streambuf signals this via @c badbit.
    [[nodiscard]] static auto streamReadError() -> Error;

    std::istream& stream_;
    std::uint64_t lineNumber_ = 0;
    std::uint64_t recordNumber_ = 0;
    std::uint64_t bytesConsumed_ = 0;
    bool eof_ = false;
    bool streamError_ = false;
};

/// 一对配对 reads；非配对模式下 `second` 为空。
struct ReadPair {
    FastqRecord first;
    std::optional<FastqRecord> second;
};

/// 读取下一条记录（配对模式下读一对）。
/// - 成功且非空：读到记录，`first` 必有，配对时 `second` 也有
/// - 成功且空：正常 EOF；配对模式下两端同时 EOF
/// - 失败：解析错误，或配对两端记录数不一致（kFormatError）
[[nodiscard]] auto readRecordPair(FastqParser& primary,
                                  FastqParser* mate) -> Result<std::optional<ReadPair>>;

}  // namespace fqc::io
