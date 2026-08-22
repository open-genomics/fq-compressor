// =============================================================================
// fq-compressor - FASTQ Parser Implementation
// =============================================================================

#include "fqc/io/fastq_parser.h"

#include <istream>
#include <string>

namespace fqc::io {

FastqParser::FastqParser(std::istream& stream) : stream_(stream) {}

auto FastqParser::readRecord() -> Result<std::optional<ReadRecord>> {
    if (eof_) {
        return std::optional<ReadRecord>{};
    }

    std::string idLine;
    if (!readLine(idLine)) {
        if (streamError_) {
            return std::unexpected(streamReadError());
        }
        return std::optional<ReadRecord>{};
    }

    while (idLine.empty()) {
        if (!readLine(idLine)) {
            if (streamError_) {
                return std::unexpected(streamReadError());
            }
            return std::optional<ReadRecord>{};
        }
    }

    if (idLine[0] != '@') {
        return std::unexpected(formatError("expected '@'"));
    }

    ReadRecord record;
    std::string_view idView(idLine);
    idView.remove_prefix(1);

    auto spacePos = idView.find(' ');
    if (spacePos != std::string_view::npos) {
        record.id = std::string(idView.substr(0, spacePos));
        // comment 保留首个分隔空格起（含该空格）的整段文本：重建时 id+comment 原样拼接，
        // 使 "@id "（仅一个尾随空格）这样的头部也能逐字节往返（无损硬约束）。仅存 comment
        // 文本、重建时补空格的旧口径会在 comment 为空时丢失该空格。
        record.comment = std::string(idView.substr(spacePos));
    } else {
        record.id = std::string(idView);
    }

    if (record.id.empty()) {
        return std::unexpected(formatError("empty read ID"));
    }

    FQC_TRY(readRequiredLine(record.sequence));
    if (record.sequence.empty()) {
        return std::unexpected(formatError("empty sequence"));
    }

    std::string plusLine;
    FQC_TRY(readRequiredLine(plusLine));
    if (plusLine.empty() || plusLine[0] != '+') {
        return std::unexpected(formatError("expected '+'"));
    }

    FQC_TRY(readRequiredLine(record.quality));
    if (record.quality.size() != record.sequence.size()) {
        return std::unexpected(formatError("quality length does not match sequence length"));
    }

    ++recordNumber_;
    return std::optional<ReadRecord>(std::move(record));
}

auto FastqParser::readLine(std::string& line) -> bool {
    if (eof_) {
        return false;
    }
    if (!std::getline(stream_, line)) {
        // badbit (not eofbit) means the underlying stream failed -- e.g. a corrupt
        // gzip member made GzipStreamBuf::underflow() throw. Distinguish this from a
        // clean end-of-file so the caller can surface an I/O error instead of silently
        // truncating the input.
        if (stream_.bad()) {
            streamError_ = true;
        }
        eof_ = true;
        return false;
    }
    ++lineNumber_;
    // Count raw bytes before trimTrailingCr strips '\r'. getline consumes the '\n'
    // delimiter unless it stopped at EOF (eofbit set after extracting content).
    bytesConsumed_ += line.size() + (stream_.eof() ? 0 : 1);
    trimTrailingCr(line);
    return true;
}

auto FastqParser::readRequiredLine(std::string& line) -> VoidResult {
    if (!readLine(line)) {
        if (streamError_) {
            return std::unexpected(streamReadError());
        }
        return makeVoidError(ErrorCode::kFormatError,
                             "invalid FASTQ: unexpected EOF at line " +
                                 std::to_string(lineNumber_));
    }
    return {};
}

auto FastqParser::formatError(std::string_view detail) const -> Error {
    return Error{ErrorCode::kFormatError,
                 "invalid FASTQ at line " + std::to_string(lineNumber_) + ": " +
                     std::string(detail)};
}

auto FastqParser::streamReadError() -> Error {
    return Error{ErrorCode::kIOError, "input stream read error while parsing FASTQ"};
}

auto readRecordPair(FastqParser& primary, FastqParser* mate) -> Result<std::optional<ReadPair>> {
    FQC_TRY_ASSIGN(first, primary.readRecord());
    if (!first.has_value()) {
        if (mate != nullptr) {
            FQC_TRY_ASSIGN(second, mate->readRecord());
            if (second.has_value()) {
                return makeError<std::optional<ReadPair>>(
                    ErrorCode::kFormatError, "paired inputs have different record counts");
            }
        }
        return std::optional<ReadPair>{};
    }
    ReadPair pair;
    pair.first = std::move(*first);
    if (mate != nullptr) {
        FQC_TRY_ASSIGN(second, mate->readRecord());
        if (!second.has_value()) {
            return makeError<std::optional<ReadPair>>(ErrorCode::kFormatError,
                                                      "paired inputs have different record counts");
        }
        pair.second = std::move(*second);
    }
    return std::optional<ReadPair>{std::move(pair)};
}

}  // namespace fqc::io
