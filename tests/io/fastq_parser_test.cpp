// =============================================================================
// fq-compressor - FASTQ Parser Tests
// =============================================================================

#include "fqc/io/fastq_parser.h"

#include "fqc/common/types.h"

#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <type_traits>

#include <gtest/gtest.h>

namespace fqc::io::test {

TEST(FastqParserTest, FastqRecordUsesSharedReadRecordType) {
    EXPECT_TRUE((std::is_same_v<FastqRecord, ReadRecord>));
}

TEST(FastqParserTest, ParsesValidRecord) {
    std::istringstream input("@read1 comment\nACGT\n+\nIIII\n");
    FastqParser parser(input);

    auto result = parser.readRecord();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((**result).id, "read1");
    EXPECT_EQ((**result).comment, "comment");
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

}  // namespace fqc::io::test
