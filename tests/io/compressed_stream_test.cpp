// =============================================================================
// fq-compressor - Compressed Stream Unit Tests
// =============================================================================
// Unit tests for transparent compression/decompression stream support.
// Tests gzip and uncompressed formats.
// =============================================================================

#include "fqc/io/compressed_stream.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace fqc::io {
namespace {

// =============================================================================
// Test Fixtures
// =============================================================================

class CompressedStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = std::filesystem::temp_directory_path() /
            ("fqc_compressed_stream_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(tempDir_);

        // Generate test data
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dist('A', 'Z');
        testData_.resize(4096);
        for (auto& c : testData_) {
            c = static_cast<char>(dist(rng));
        }
    }

    void TearDown() override {
        std::filesystem::remove_all(tempDir_);
    }

    std::filesystem::path tempDir_;
    std::string testData_;

    // Helper to write test data to a file
    void writeTestFile(const std::filesystem::path& path, const std::string& data) {
        std::ofstream file(path, std::ios::binary);
        file << data;
    }

    // Helper to read entire file content
    std::string readFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

// =============================================================================
// Compression Format Detection Tests
// =============================================================================

TEST_F(CompressedStreamTest, DetectNoneFormat) {
    // Plain text doesn't match any compression magic
    std::string plain = "Hello, World!";
    auto format = detectCompressionFormat(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()));
    EXPECT_EQ(format, CompressionFormat::kNone);
}

TEST_F(CompressedStreamTest, DetectGzipMagic) {
    // gzip magic bytes: 0x1f 0x8b
    std::uint8_t gzipMagic[] = {0x1f, 0x8b, 0x08, 0x00};
    auto format = detectCompressionFormat(gzipMagic);
    EXPECT_EQ(format, CompressionFormat::kGzip);
}

TEST_F(CompressedStreamTest, DetectBzip2Magic) {
    // bzip2 magic: "BZ"
    std::uint8_t bzip2Magic[] = {0x42, 0x5a, 0x68, 0x39};  // "BZh9"
    auto format = detectCompressionFormat(bzip2Magic);
    EXPECT_EQ(format, CompressionFormat::kBzip2);
}

TEST_F(CompressedStreamTest, DetectXzMagic) {
    // xz magic: 0xFD "7zXZ"
    std::uint8_t xzMagic[] = {0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00};
    auto format = detectCompressionFormat(xzMagic);
    EXPECT_EQ(format, CompressionFormat::kXz);
}

TEST_F(CompressedStreamTest, DetectFromExtension) {
    EXPECT_EQ(detectCompressionFormatFromExtension("file.gz"), CompressionFormat::kGzip);
    EXPECT_EQ(detectCompressionFormatFromExtension("file.GZ"), CompressionFormat::kGzip);
    EXPECT_EQ(detectCompressionFormatFromExtension("file.bz2"), CompressionFormat::kBzip2);
    EXPECT_EQ(detectCompressionFormatFromExtension("file.BZ2"), CompressionFormat::kBzip2);
    EXPECT_EQ(detectCompressionFormatFromExtension("file.xz"), CompressionFormat::kXz);
    EXPECT_EQ(detectCompressionFormatFromExtension("file.XZ"), CompressionFormat::kXz);
    EXPECT_EQ(detectCompressionFormatFromExtension("file.fastq"), CompressionFormat::kNone);
    EXPECT_EQ(detectCompressionFormatFromExtension("file.txt"), CompressionFormat::kNone);
}

TEST_F(CompressedStreamTest, FormatName) {
    EXPECT_EQ(compressionFormatName(CompressionFormat::kGzip), "gzip");
    EXPECT_EQ(compressionFormatName(CompressionFormat::kBzip2), "bzip2");
    EXPECT_EQ(compressionFormatName(CompressionFormat::kXz), "xz");
    EXPECT_EQ(compressionFormatName(CompressionFormat::kNone), "none");
}

// =============================================================================
// Uncompressed File Tests
// =============================================================================

TEST_F(CompressedStreamTest, ReadUncompressedFile) {
    auto path = tempDir_ / "test.txt";
    writeTestFile(path, testData_);

    CompressedInputStream stream(path);
    EXPECT_EQ(stream.format(), CompressionFormat::kNone);
    EXPECT_FALSE(stream.isCompressed());

    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_EQ(buffer.str(), testData_);
}

TEST_F(CompressedStreamTest, OpenCompressedFileUncompressed) {
    auto path = tempDir_ / "test.txt";
    writeTestFile(path, testData_);

    auto result = openCompressedFile(path);
    ASSERT_TRUE(result.has_value());
    std::stringstream buffer;
    buffer << (*result)->rdbuf();
    EXPECT_EQ(buffer.str(), testData_);
}

// =============================================================================
// Gzip Tests
// =============================================================================

TEST_F(CompressedStreamTest, ReadGzipFile) {
    auto compressedPath = tempDir_ / "test.gz";

    // Create gzip file using system command
    {
        auto plainPath = tempDir_ / "test_plain.txt";
        writeTestFile(plainPath, testData_);
        std::string cmd = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
        ASSERT_EQ(system(cmd.c_str()), 0);
    }

    CompressedInputStream stream(compressedPath);
    EXPECT_EQ(stream.format(), CompressionFormat::kGzip);
    EXPECT_TRUE(stream.isCompressed());

    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_EQ(buffer.str(), testData_);
}

TEST_F(CompressedStreamTest, OpenCompressedFileGzip) {
    auto compressedPath = tempDir_ / "test.gz";

    {
        auto plainPath = tempDir_ / "test_plain.txt";
        writeTestFile(plainPath, testData_);
        std::string cmd = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
        ASSERT_EQ(system(cmd.c_str()), 0);
    }

    auto result = openCompressedFile(compressedPath);
    ASSERT_TRUE(result.has_value());
    std::stringstream buffer;
    buffer << (*result)->rdbuf();
    EXPECT_EQ(buffer.str(), testData_);
}

TEST_F(CompressedStreamTest, ReadsConcatenatedGzipMembers) {
    const auto firstPlain = tempDir_ / "first.txt";
    const auto secondPlain = tempDir_ / "second.txt";
    const auto firstGzip = tempDir_ / "first.gz";
    const auto secondGzip = tempDir_ / "second.gz";
    const auto concatenated = tempDir_ / "concatenated.gz";
    writeTestFile(firstPlain, "first member\n");
    writeTestFile(secondPlain, "second member\n");

    const auto firstCommand = "gzip -c " + firstPlain.string() + " > " + firstGzip.string();
    const auto secondCommand = "gzip -c " + secondPlain.string() + " > " + secondGzip.string();
    ASSERT_EQ(system(firstCommand.c_str()), 0);
    ASSERT_EQ(system(secondCommand.c_str()), 0);
    writeTestFile(concatenated, readFile(firstGzip) + readFile(secondGzip));

    CompressedInputStream stream(concatenated);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_EQ(buffer.str(), "first member\nsecond member\n");
}

// =============================================================================
// GzipStreamBuf Tests
// =============================================================================

/// Serves a fixed byte range once, then simulates a hard I/O failure: the
/// next refill throws, which istreams translate into badbit without eofbit.
class FailingSourceBuf : public std::streambuf {
public:
    explicit FailingSourceBuf(std::string data) : data_(std::move(data)) {
        setg(data_.data(), data_.data(), data_.data() + data_.size());
    }

protected:
    int_type underflow() override {
        throw std::runtime_error("simulated I/O failure");
    }

private:
    std::string data_;
};

class GzipStreamBufTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = std::filesystem::temp_directory_path() /
            ("fqc_gzip_streambuf_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tempDir_);
    }

    std::filesystem::path tempDir_;
};

TEST_F(GzipStreamBufTest, DecompressGzipStream) {
    auto compressedPath = tempDir_ / "test.gz";
    std::string testData(1024, 'X');

    // Create gzip file
    {
        auto plainPath = tempDir_ / "test_plain.txt";
        {
            std::ofstream file(plainPath);
            file << testData;
        }
        std::string cmd = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
        ASSERT_EQ(system(cmd.c_str()), 0);
    }

    std::ifstream compressedFile(compressedPath, std::ios::binary);
    ASSERT_TRUE(compressedFile.is_open());

    GzipStreamBuf streamBuf(compressedFile);
    std::istream stream(&streamBuf);

    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_EQ(buffer.str(), testData);
}

TEST_F(GzipStreamBufTest, MoveSemantics) {
    auto compressedPath = tempDir_ / "test.gz";
    std::string testData(512, 'A');

    {
        auto plainPath = tempDir_ / "test_plain.txt";
        {
            std::ofstream file(plainPath);
            file << testData;
        }
        std::string cmd = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
        ASSERT_EQ(system(cmd.c_str()), 0);
    }

    std::ifstream compressedFile(compressedPath, std::ios::binary);
    GzipStreamBuf streamBuf1(compressedFile);

    // Move construct
    GzipStreamBuf streamBuf2(std::move(streamBuf1));

    // Move assign
    GzipStreamBuf streamBuf3;
    streamBuf3 = std::move(streamBuf2);

    std::istream stream(&streamBuf3);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_EQ(buffer.str(), testData);
}

TEST_F(GzipStreamBufTest, UnderlyingFailureSurfacesInsteadOfSilentEof) {
    // A source stream that dies mid-stream must surface as an error, not as
    // a clean EOF that silently truncates the decompressed payload.
    std::string testData(256 * 1024, '\0');
    std::mt19937 rng(7);
    for (char& c : testData) {
        c = static_cast<char>(rng());
    }
    auto compressedPath = tempDir_ / "failing.gz";
    {
        auto plainPath = tempDir_ / "failing_plain.txt";
        std::ofstream file(plainPath, std::ios::binary);
        file << testData;
        std::string cmd = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
        ASSERT_EQ(system(cmd.c_str()), 0);
    }
    std::string compressed;
    {
        std::ifstream in(compressedPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        compressed = ss.str();
    }
    ASSERT_GT(compressed.size(), std::size_t{64} * 1024);
    compressed.resize(compressed.size() / 2);  // cut the source mid-stream

    FailingSourceBuf sourceBuf(compressed);
    std::istream source(&sourceBuf);
    GzipStreamBuf gzipBuf(source);
    std::istream stream(&gzipBuf);

    std::string decoded;
    char chunk[4096];
    while (stream.read(chunk, sizeof(chunk)) || stream.gcount() > 0) {
        decoded.append(chunk, static_cast<std::size_t>(stream.gcount()));
        if (stream.bad()) {
            break;
        }
    }
    EXPECT_TRUE(stream.bad());
    EXPECT_LT(decoded.size(), testData.size());
    EXPECT_EQ(testData.compare(0, decoded.size(), decoded), 0);
}

TEST_F(GzipStreamBufTest, MoveAssignmentPreservesBufferedOutput) {
    const auto compressedPath = tempDir_ / "buffered.gz";
    const std::string testData(200'000, 'B');
    const auto plainPath = tempDir_ / "buffered.txt";
    {
        std::ofstream output(plainPath, std::ios::binary);
        output << testData;
    }
    const auto command = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
    ASSERT_EQ(system(command.c_str()), 0);

    std::ifstream compressedFile(compressedPath, std::ios::binary);
    GzipStreamBuf source(compressedFile);
    std::istream first(&source);
    std::string prefix(17, '\0');
    first.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    ASSERT_EQ(prefix, testData.substr(0, prefix.size()));

    GzipStreamBuf destination;
    destination = std::move(source);
    std::istream remainder(&destination);
    std::stringstream buffer;
    buffer << remainder.rdbuf();
    EXPECT_EQ(prefix + buffer.str(), testData);
}

// =============================================================================
// Compression Support Query Tests
// =============================================================================

TEST_F(CompressedStreamTest, IsCompressionSupported) {
    EXPECT_TRUE(isCompressionSupported(CompressionFormat::kNone));
    EXPECT_TRUE(isCompressionSupported(CompressionFormat::kGzip));
    EXPECT_FALSE(isCompressionSupported(CompressionFormat::kBzip2));
    EXPECT_FALSE(isCompressionSupported(CompressionFormat::kXz));
    EXPECT_FALSE(isCompressionSupported(CompressionFormat::kZstd));
}

// =============================================================================
// Edge Cases and Error Handling
// =============================================================================

TEST_F(CompressedStreamTest, EmptyFile) {
    auto path = tempDir_ / "empty.txt";
    writeTestFile(path, "");

    CompressedInputStream stream(path);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_TRUE(buffer.str().empty());
}

TEST_F(CompressedStreamTest, EmptyGzipFile) {
    auto compressedPath = tempDir_ / "empty.gz";

    {
        auto plainPath = tempDir_ / "empty.txt";
        writeTestFile(plainPath, "");
        std::string cmd = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
        ASSERT_EQ(system(cmd.c_str()), 0);
    }

    CompressedInputStream stream(compressedPath);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_TRUE(buffer.str().empty());
}

TEST_F(CompressedStreamTest, LargeFile) {
    // Test with larger data to exercise buffer handling
    std::string largeData(1024 * 1024, 'L');  // 1MB
    auto compressedPath = tempDir_ / "large.gz";

    {
        auto plainPath = tempDir_ / "large.txt";
        writeTestFile(plainPath, largeData);
        std::string cmd = "gzip -c " + plainPath.string() + " > " + compressedPath.string();
        ASSERT_EQ(system(cmd.c_str()), 0);
    }

    CompressedInputStream stream(compressedPath);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    EXPECT_EQ(buffer.str(), largeData);
}

TEST_F(CompressedStreamTest, NonexistentFile) {
    EXPECT_THROW(CompressedInputStream stream(tempDir_ / "nonexistent.txt"), std::runtime_error);
}

TEST_F(CompressedStreamTest, InvalidGzipData) {
    auto path = tempDir_ / "invalid.gz";
    {
        std::ofstream file(path, std::ios::binary);
        // Write invalid gzip data (looks like gzip but isn't)
        file << "\x1f\x8b\x08\x00";
        file << "this is not valid gzip compressed data";
    }

    CompressedInputStream stream(path);
    (void)stream.peek();
    EXPECT_TRUE(stream.bad() || stream.fail() || stream.eof());
}

// =============================================================================
// Integration Tests
// =============================================================================

TEST_F(CompressedStreamTest, RoundTripGzip) {
    std::vector<std::pair<std::string, std::string>> formats = {
        {"gzip", ".gz"},
    };

    for (const auto& [formatName, ext] : formats) {
        auto compressedPath = tempDir_ / ("test_" + formatName + ext);

        {
            auto plainPath = tempDir_ / "test_plain.txt";
            writeTestFile(plainPath, testData_);
            std::string cmd =
                formatName + " -c " + plainPath.string() + " > " + compressedPath.string();
            if (system(("which " + formatName + " > /dev/null 2>&1").c_str()) != 0) {
                continue;  // Skip if compressor not available
            }
            ASSERT_EQ(system(cmd.c_str()), 0);
        }

        auto result = openCompressedFile(compressedPath);
        ASSERT_TRUE(result.has_value());
        std::stringstream buffer;
        buffer << (*result)->rdbuf();
        EXPECT_EQ(buffer.str(), testData_) << "Failed for format: " << formatName;
    }
}

}  // namespace

TEST_F(CompressedStreamTest, EmptyFileWithGzipExtensionReadsAsCleanEof) {
    // A 0-byte input named ".gz" is an empty file, not a truncated gzip
    // member: it must open as plain empty input and read to a clean EOF
    // instead of surfacing an I/O/format error from the gzip path.
    auto path = tempDir_ / "empty.fastq.gz";
    writeTestFile(path, "");

    auto result = openCompressedFile(path);
    ASSERT_TRUE(result.has_value());

    // Read the way a FASTQ parser does (getline) so a failing gzip layer
    // would surface as badbit; an empty plain file must read to a clean EOF
    // (getline on empty input sets eof|fail, but never bad).
    auto& stream = **result;
    std::string line;
    std::getline(stream, line);
    EXPECT_TRUE(line.empty());
    EXPECT_FALSE(stream.bad());
    EXPECT_TRUE(stream.eof());
}

}  // namespace fqc::io
