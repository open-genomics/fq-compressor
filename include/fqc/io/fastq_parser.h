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

private:
    [[nodiscard]] auto readLine(std::string& line) -> bool;
    static void trimRight(std::string& str);
    /// @brief Error returned when the underlying stream fails (e.g. corrupt gzip input),
    /// as opposed to a clean end-of-file. The streambuf signals this via @c badbit.
    [[nodiscard]] static auto streamReadError() -> Error;

    std::istream& stream_;
    std::uint64_t lineNumber_ = 0;
    std::uint64_t recordNumber_ = 0;
    bool eof_ = false;
    bool streamError_ = false;
};

}  // namespace fqc::io
