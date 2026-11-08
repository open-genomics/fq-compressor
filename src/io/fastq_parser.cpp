// =============================================================================
// fq-compressor - FASTQ Parser Implementation
// =============================================================================

#include "fqc/io/fastq_parser.h"

#include <array>
#include <istream>
#include <limits>
#include <string>

namespace fqc::io {

FastqParser::FastqParser(std::istream& stream, std::size_t maxLineBytes)
    : stream_(stream), maxLineBytes_(maxLineBytes) {}

auto FastqParser::readRecord() -> Result<std::optional<ReadRecord>> {
    if (eof_) {
        return std::optional<ReadRecord>{};
    }

    auto& idLine = idLine_;
    FQC_TRY_ASSIGN(hasHeader, readLineChecked(idLine));
    if (!hasHeader) {
        return std::optional<ReadRecord>{};
    }

    while (idLine.empty()) {
        FQC_TRY_ASSIGN(hasNextHeader, readLineChecked(idLine));
        if (!hasNextHeader) {
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

    auto& plusLine = plusLine_;
    FQC_TRY(readRequiredLine(plusLine));
    if (plusLine.empty() || plusLine[0] != '+') {
        return std::unexpected(formatError("expected '+'"));
    }
    // 归档只重建裸 '+'；接受 "+id"/"+desc" 会让解压输出静默改写该行，
    // 违反字节级无损承诺。显式拒绝，而不是悄悄丢弃这段文本。
    if (plusLine.size() != 1) {
        return std::unexpected(formatError(
            "'+' line must be bare; per-read '+' content is not stored by the archive format"));
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

    // 分块读行：std::getline(string) 会先按整行长度分配内存，gzip 解压出的
    // 超长行可绕过 maxLineBytes_ 把内存打满。固定 64 KiB 栈缓冲逐块扫描，
    // 只有不超过上限的内容才追加到 line。
    constexpr std::size_t kChunkBytes = 64 * 1024;
    std::array<char, kChunkBytes> chunk{};
    line.clear();
    std::size_t rawBytes = 0;
    bool readAny = false;

    while (true) {
        stream_.getline(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto extracted = static_cast<std::size_t>(stream_.gcount());
        // getline 在「读到 EOF 但已取到字符」时只置 eofbit、不置 failbit；
        // 只有真正遇到 '\n' 才是分隔符。把 eof 当成分隔符会吃掉最后一行的
        // 末字符（无尾随换行的合法记录会被截断）。
        const bool hasDelimiter = !stream_.fail() && !stream_.eof();
        const std::size_t contentBytes = extracted - (hasDelimiter && extracted > 0 ? 1U : 0U);

        rawBytes += extracted;
        if (contentBytes > 0) {
            if (line.size() + contentBytes > maxLineBytes_) {
                lineTooLong_ = true;
                // 排空剩余行，保持错误路径之后的流位置可预测；ignore 不分配内存。
                stream_.clear();
                stream_.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                bytesConsumed_ += rawBytes;
                return false;
            }
            line.append(chunk.data(), contentBytes);
            readAny = true;
        }

        if (hasDelimiter) {
            break;
        }
        if (stream_.eof()) {
            break;
        }
        if (stream_.bad()) {
            streamError_ = true;
            eof_ = true;
            bytesConsumed_ += rawBytes;
            return false;
        }
        // 未遇到分隔符且未到 EOF：缓冲区满，清除 failbit 后继续下一块。
        stream_.clear();
    }

    if (!readAny && rawBytes == 0) {
        eof_ = true;
        return false;
    }

    ++lineNumber_;
    bytesConsumed_ += rawBytes;
    trimTrailingCr(line);
    return true;
}

auto FastqParser::readLineChecked(std::string& line) -> Result<bool> {
    if (readLine(line)) {
        return true;
    }
    if (lineTooLong_) {
        return makeError<bool>(formatError("FASTQ line exceeds maximum length of " +
                                           std::to_string(maxLineBytes_) + " bytes"));
    }
    if (streamError_) {
        return makeError<bool>(streamReadError());
    }
    return false;
}

auto FastqParser::readRequiredLine(std::string& line) -> VoidResult {
    FQC_TRY_ASSIGN(hasLine, readLineChecked(line));
    if (!hasLine) {
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
