// =============================================================================
// fq-compressor - FASTQ Parser Implementation
// =============================================================================

#include "fqc/io/fastq_parser.h"

#include <istream>
#include <string>

namespace fqc::io {

FastqParser::FastqParser(std::istream& stream) : stream_(stream) {}

auto FastqParser::readRecord() -> Result<std::optional<FastqRecord>> {
    if (eof_) {
        return std::optional<FastqRecord>{};
    }

    std::string idLine;
    if (!readLine(idLine)) {
        if (streamError_) {
            return makeError<std::optional<FastqRecord>>(streamReadError());
        }
        return std::optional<FastqRecord>{};
    }

    while (idLine.empty()) {
        if (!readLine(idLine)) {
            if (streamError_) {
                return makeError<std::optional<FastqRecord>>(streamReadError());
            }
            return std::optional<FastqRecord>{};
        }
    }

    if (idLine[0] != '@') {
        return makeError<std::optional<FastqRecord>>(
            ErrorCode::kFormatError,
            "invalid FASTQ at line " + std::to_string(lineNumber_) + ": expected '@'");
    }

    FastqRecord record;
    std::string_view idView(idLine);
    idView.remove_prefix(1);

    auto spacePos = idView.find(' ');
    if (spacePos != std::string_view::npos) {
        record.id = std::string(idView.substr(0, spacePos));
        record.comment = std::string(idView.substr(spacePos + 1));
    } else {
        record.id = std::string(idView);
    }

    if (record.id.empty()) {
        return makeError<std::optional<FastqRecord>>(
            ErrorCode::kFormatError,
            "invalid FASTQ at line " + std::to_string(lineNumber_) + ": empty read ID");
    }

    if (!readLine(record.sequence)) {
        if (streamError_) {
            return makeError<std::optional<FastqRecord>>(streamReadError());
        }
        return makeError<std::optional<FastqRecord>>(ErrorCode::kFormatError,
                                                     "invalid FASTQ: unexpected EOF at line " +
                                                         std::to_string(lineNumber_));
    }

    if (record.sequence.empty()) {
        return makeError<std::optional<FastqRecord>>(
            ErrorCode::kFormatError,
            "invalid FASTQ at line " + std::to_string(lineNumber_) + ": empty sequence");
    }

    std::string plusLine;
    if (!readLine(plusLine)) {
        if (streamError_) {
            return makeError<std::optional<FastqRecord>>(streamReadError());
        }
        return makeError<std::optional<FastqRecord>>(ErrorCode::kFormatError,
                                                     "invalid FASTQ: unexpected EOF at line " +
                                                         std::to_string(lineNumber_));
    }

    if (plusLine.empty() || plusLine[0] != '+') {
        return makeError<std::optional<FastqRecord>>(
            ErrorCode::kFormatError,
            "invalid FASTQ at line " + std::to_string(lineNumber_) + ": expected '+'");
    }

    if (!readLine(record.quality)) {
        if (streamError_) {
            return makeError<std::optional<FastqRecord>>(streamReadError());
        }
        return makeError<std::optional<FastqRecord>>(ErrorCode::kFormatError,
                                                     "invalid FASTQ: unexpected EOF at line " +
                                                         std::to_string(lineNumber_));
    }

    if (record.quality.size() != record.sequence.size()) {
        return makeError<std::optional<FastqRecord>>(
            ErrorCode::kFormatError,
            "invalid FASTQ at line " + std::to_string(lineNumber_) +
                ": quality length does not match sequence length");
    }

    ++recordNumber_;
    return std::optional<FastqRecord>(std::move(record));
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
    trimRight(line);
    return true;
}

auto FastqParser::streamReadError() -> Error {
    return Error{ErrorCode::kIOError, "input stream read error while parsing FASTQ"};
}

void FastqParser::trimRight(std::string& str) {
    while (!str.empty() && str.back() == '\r') {
        str.pop_back();
    }
}

}  // namespace fqc::io
