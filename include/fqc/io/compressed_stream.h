// =============================================================================
// fq-compressor - Compressed Stream Support
// =============================================================================
// Transparent gzip decompression for compressed input files.
//
// Only gzip (.gz) is decompressed. bzip2/xz/zstd magic bytes are still detected
// so an unsupported input is rejected (fail-closed) rather than silently read
// as plain text.
//
// Usage:
//   auto stream = io::openInputFile("/path/to/reads.fastq.gz");
//   // Use *stream like any std::istream
// =============================================================================

#pragma once

#include "fqc/common/error.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <span>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace fqc::io {

// =============================================================================
// Compression Format Detection
// =============================================================================

/// @brief Compression formats recognised on input. Only kGzip can be
/// decompressed; the others are detected so they can be rejected explicitly.
enum class CompressionFormat : std::uint8_t {
    kNone = 0,   ///< Uncompressed (plain text)
    kGzip = 1,   ///< gzip (.gz) - decompressed in-process via zlib
    kBzip2 = 2,  ///< bzip2 (.bz2) - detected, not supported
    kXz = 3,     ///< xz/lzma (.xz) - detected, not supported
    kZstd = 4,   ///< zstd (.zst) - detected, not supported
    kUnknown = 255
};

/// @brief Detect compression format from file magic bytes.
/// @param data First few bytes of the file.
/// @return Detected compression format.
[[nodiscard]] CompressionFormat detectCompressionFormat(std::span<const std::uint8_t> data);

/// @brief Detect compression format from file extension.
/// @param path File path.
/// @return Detected compression format.
[[nodiscard]] CompressionFormat detectCompressionFormatFromExtension(
    const std::filesystem::path& path);

/// @brief Get human-readable name for compression format.
/// @param format Compression format.
/// @return Format name (e.g., "gzip").
[[nodiscard]] std::string_view compressionFormatName(CompressionFormat format);

// =============================================================================
// GzipStreamBuf
// =============================================================================

/// @brief Stream buffer for gzip decompression.
/// @note Uses zlib for streaming decompression.
/// @note Internal implementation detail; kept public only so the move
/// semantics (see the gzip-streambuf-move-loss postmortem) can be unit-tested.
class GzipStreamBuf : public std::streambuf {
public:
    /// @brief Default constructor (for move semantics).
    GzipStreamBuf() = default;

    /// @brief Construct a gzip stream buffer.
    /// @param source Source stream to decompress.
    /// @param bufferSize Internal buffer size.
    explicit GzipStreamBuf(std::istream& source, std::size_t bufferSize = std::size_t{64} * 1024);

    /// @brief Destructor.
    ~GzipStreamBuf() override;

    // Non-copyable
    GzipStreamBuf(const GzipStreamBuf&) = delete;
    GzipStreamBuf& operator=(const GzipStreamBuf&) = delete;

    // Movable
    GzipStreamBuf(GzipStreamBuf&& other) noexcept;
    GzipStreamBuf& operator=(GzipStreamBuf&& other) noexcept;

protected:
    /// @brief Underflow handler - refill buffer.
    int_type underflow() override;

private:
    /// @brief Initialize zlib stream.
    void initZlib();

    /// @brief Cleanup zlib stream.
    void cleanupZlib();

    /// @brief Decompress more data into output buffer.
    /// @return Number of bytes decompressed.
    std::size_t decompress();

    /// @brief Source stream.
    std::istream* source_ = nullptr;

    /// @brief Compressed input buffer.
    std::vector<std::uint8_t> inputBuffer_;

    /// @brief Decompressed output buffer.
    std::vector<char> outputBuffer_;

    /// @brief zlib stream state (opaque pointer).
    void* zlibStream_ = nullptr;

    /// @brief Whether stream is initialized.
    bool initialized_ = false;

    /// @brief Whether end of compressed stream reached.
    bool streamEnd_ = false;
};

// =============================================================================
// CompressedInputStream
// =============================================================================

/// @brief Input stream with transparent decompression.
class CompressedInputStream : public std::istream {
public:
    /// @brief Construct from a file path.
    /// @param path File path.
    /// @throws IOError if file cannot be opened.
    explicit CompressedInputStream(const std::filesystem::path& path);

    /// @brief Construct from an existing stream.
    /// @param source Source stream.
    /// @param format Compression format (must be known; detect beforehand via
    ///                detectCompressionFormat()).
    explicit CompressedInputStream(std::unique_ptr<std::istream> source, CompressionFormat format);

    /// @brief Destructor.
    ~CompressedInputStream() override;

    // Non-copyable, non-movable (std::istream is not movable)
    CompressedInputStream(const CompressedInputStream&) = delete;
    CompressedInputStream& operator=(const CompressedInputStream&) = delete;
    CompressedInputStream(CompressedInputStream&&) = delete;
    CompressedInputStream& operator=(CompressedInputStream&&) = delete;

    /// @brief Get the detected compression format.
    [[nodiscard]] CompressionFormat format() const noexcept {
        return format_;
    }

    /// @brief Check if the stream is compressed.
    [[nodiscard]] bool isCompressed() const noexcept {
        return format_ != CompressionFormat::kNone;
    }

private:
    /// @brief Detect format and setup decompression.
    void setup();

    /// @brief Source file stream (if opened from path).
    std::unique_ptr<std::ifstream> fileStream_;

    /// @brief Source stream.
    std::unique_ptr<std::istream> sourceStream_;

    /// @brief Decompression stream buffer.
    std::unique_ptr<std::streambuf> decompressBuf_;

    /// @brief Detected compression format.
    CompressionFormat format_ = CompressionFormat::kUnknown;
};

// =============================================================================
// Factory Functions
// =============================================================================

/// @brief Open a file with automatic decompression.
/// @param path File path.
/// @return Input stream (decompressing if needed), or error.
[[nodiscard]] auto openCompressedFile(const std::filesystem::path& path)
    -> Result<std::unique_ptr<std::istream>>;

/// @brief Open a file or stdin with automatic decompression.
/// @param path File path (or "-" for stdin).
/// @return Input stream (decompressing if needed), or error.
[[nodiscard]] auto openInputFile(const std::filesystem::path& path)
    -> Result<std::unique_ptr<std::istream>>;

// =============================================================================
// Compression Support Query
// =============================================================================

/// @brief Check if a compression format is supported.
/// @param format Compression format.
/// @return true if format is supported for decompression.
[[nodiscard]] bool isCompressionSupported(CompressionFormat format) noexcept;

}  // namespace fqc::io
