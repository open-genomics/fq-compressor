// =============================================================================
// fq-compressor - FASTQ Parser Tests
// =============================================================================

#include "fqc/io/fastq_parser.h"

#include "fqc/common/types.h"

#include <sstream>
#include <stdexcept>
#include <streambuf>

#include <gtest/gtest.h>

namespace fqc::io::test {

TEST(FastqParserTest, ParsesValidRecord) {
    std::istringstream input("@read1 comment\nACGT\n+\nIIII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((**result).id, "read1");
    EXPECT_EQ((**result).comment, " comment");
    EXPECT_EQ((**result).sequence, "ACGT");
    EXPECT_EQ((**result).quality, "IIII");
}

TEST(FastqParserTest, ReturnsNulloptAtEof) {
    std::istringstream input("@read1\nACGT\n+\nIIII\n");
    FastqParser parser(input);

    auto first = parser.readRecord();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());

    auto second = parser.readRecord();
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->has_value());
}

TEST(FastqParserTest, RejectsMissingAtSign) {
    std::istringstream input("read1\nACGT\n+\nIIII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
}

TEST(FastqParserTest, RejectsQualityLengthMismatch) {
    std::istringstream input("@read1\nACGT\n+\nII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
}

TEST(FastqParserTest, RejectsOversizedLineWithFormatError) {
    // 行长上限把畸形/敌意输入的超长行变成干净的 kFormatError，而不是让
    // std::string 无限增长直到 bad_alloc（在 worker 线程里会 std::terminate）。
    // 序列行 100 字节，上限 32：前一条记录正常解析，超长行报错而非被当作 EOF
    // 静默截断。
    std::istringstream input("@read1\nACGT\n+\nIIII\n@read2\n" + std::string(100, 'A') + "\n+\n" +
                             std::string(100, 'I') + "\n");
    FastqParser parser(input, /*maxLineBytes=*/32);

    auto first = parser.readRecord();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());

    auto second = parser.readRecord();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::kFormatError);
    EXPECT_NE(second.error().message.find("maximum length"), std::string::npos);
}

TEST(FastqParserTest, OversizedHeaderLineIsAlsoRejected) {
    // 上限作用于所有行（含头部行），且 bytesConsumed 记账不受影响。
    std::istringstream input("@" + std::string(64, 'x') + "\nACGT\n+\nIIII\n");
    FastqParser parser(input, /*maxLineBytes=*/32);

    auto result = parser.readRecord();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
}

TEST(FastqParserTest, AcceptsUpperAndLowerCaseIupacSequenceSymbols) {
    std::istringstream input(
        "@read1\nACGTRYSWKMBDHVNacgtryswkmbdhvn\n+\n"
        "IIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((**result).sequence, "ACGTRYSWKMBDHVNacgtryswkmbdhvn");
}

TEST(FastqParserTest, SkipsLeadingEmptyLines) {
    std::istringstream input("\n\n@read1\nACGT\n+\nIIII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((**result).id, "read1");
}

TEST(FastqParserTest, TracksLineAndRecordNumbers) {
    std::istringstream input("@r1\nACGT\n+\nIIII\n@r2\nTGCA\n+\nJJJJ\n");
    FastqParser parser(input);

    auto first = parser.readRecord();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(parser.recordNumber(), 1U);

    auto second = parser.readRecord();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(parser.recordNumber(), 2U);
}

TEST(FastqParserTest, ReportsIOErrorWhenUnderlyingStreamFails) {
    // A streambuf whose underflow throws, mimicking GzipStreamBuf on corrupt gzip input.
    // iostream catches the exception and sets badbit; the parser must surface this as an
    // I/O error rather than a clean end-of-file (which would silently truncate the input).
    struct ThrowingStreamBuf : std::streambuf {
        int_type underflow() override {
            throw std::runtime_error("gzip decompression failed");
        }
    };
    ThrowingStreamBuf buf;
    std::istream stream(&buf);
    FastqParser parser(stream);

    auto result = parser.readRecord();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kIOError);
}

// BUG-1 regression: a header ending in exactly one trailing space must not
// drop that space on round trip. The comment field stores everything from the
// first separator space (inclusive), so "@r1 " keeps its trailing space.
TEST(FastqParserTest, PreservesTrailingSeparatorInComment) {
    std::istringstream input("@r1 \nACGT\n+\nIIII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((**result).id, "r1");
    EXPECT_EQ((**result).comment, " ");
}

// Multiple separators stay verbatim inside the comment as well.
TEST(FastqParserTest, CommentsIncludeSeparatorSpaces) {
    std::istringstream input("@r1   x\nACGT\n+\nIIII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((**result).id, "r1");
    EXPECT_EQ((**result).comment, "   x");
}

// BUG-3 regression: trimTrailingCr converts a CRLF line end, so it strips
// exactly one trailing \r. A genuine trailing \r already present in the data
// is not a line-ending artifact and must survive.
TEST(FastqParserTest, StripsOnlySingleLineEndingCarriageReturn) {
    std::string line = "SEQ\r\r";
    trimTrailingCr(line);
    EXPECT_EQ(line, "SEQ\r");
}

// The final record may end without a trailing newline (EOF right after the
// quality line). That is a clean end, not truncation.
TEST(FastqParserTest, AcceptsFinalRecordWithoutTrailingNewline) {
    std::istringstream input("@read1\nACGT\n+\nIIII");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((**result).id, "read1");
    EXPECT_EQ((**result).sequence, "ACGT");
    EXPECT_EQ((**result).quality, "IIII");

    auto eof = parser.readRecord();
    ASSERT_TRUE(eof.has_value());
    EXPECT_FALSE(eof->has_value());
}

// Empty sequence (or a length-zero quality) is rejected: the parser is
// fail-closed on structurally degenerate records rather than emitting records
// that downstream encoders would reject anyway.
TEST(FastqParserTest, RejectsEmptySequenceAndQuality) {
    std::istringstream input("@read1\n\n+\n\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::kFormatError);
}

// A quality line beginning with '+' is legal (quality chars are '!'..'~'), and
// it must NOT be mistaken for the '+' separator line of a following record:
// the separator is the line immediately after the sequence, so the next record
// still parses correctly.
TEST(FastqParserTest, QualityLineStartingWithPlusIsNotAHeader) {
    std::istringstream input("@read1\nACGT\n+\n+abc\n@read2\nTGCA\n+\nJJJJ\n");
    FastqParser parser(input);

    auto first = parser.readRecord();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ((**first).quality, "+abc");

    auto second = parser.readRecord();
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->has_value());
    EXPECT_EQ((**second).id, "read2");
}

// Paired-mode asymmetry in the other direction: the mate has MORE records
// than the primary. readRecordPair must fail loudly (kFormatError) instead of
// silently dropping the surplus mate records.
TEST(FastqParserTest, RejectsMateWithMoreRecordsThanPrimary) {
    std::istringstream primary("@r1/1\nACGT\n+\nIIII\n");
    std::istringstream mate("@r1/2\nTGCA\n+\nJJJJ\n@r2/2\nACGT\n+\nJJJJ\n");
    FastqParser primaryParser(primary);
    FastqParser mateParser(mate);

    auto first = readRecordPair(primaryParser, &mateParser);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());

    auto second = readRecordPair(primaryParser, &mateParser);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::kFormatError);
}

}  // namespace fqc::io::test
