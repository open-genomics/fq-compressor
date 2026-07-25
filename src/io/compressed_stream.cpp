// =============================================================================
// fq-compressor - Compressed Stream Implementation
// =============================================================================
// Transparent decompression support implementation.
//
// Supported formats: gzip (zlib)
//
// Requirements: 1.1.1
// =============================================================================

#include "fqc/io/compressed_stream.h"

#include "fqc/log.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <zlib.h>

namespace fqc::io {

// =============================================================================
// Magic Bytes for Format Detection
// =============================================================================

namespace {

// Gzip magic: 0x1f 0x8b
constexpr std::uint8_t kGzipMagic[] = {0x1f, 0x8b};

// Bzip2 magic: 'B' 'Z' 'h'
constexpr std::uint8_t kBzip2Magic[] = {0x42, 0x5a, 0x68};

// XZ magic: 0xfd '7' 'z' 'X' 'Z' 0x00
constexpr std::uint8_t kXzMagic[] = {0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00};

// Zstd magic: 0x28 0xb5 0x2f 0xfd
constexpr std::uint8_t kZstdMagic[] = {0x28, 0xb5, 0x2f, 0xfd};

}  // namespace

// =============================================================================
// Format Detection
// =============================================================================

CompressionFormat detectCompressionFormat(std::span<const std::uint8_t> data) {
    if (data.size() < 2) {
        return CompressionFormat::kNone;
    }

    // Check gzip (most common for FASTQ)
    if (data.size() >= sizeof(kGzipMagic) &&
        std::memcmp(data.data(), kGzipMagic, sizeof(kGzipMagic)) == 0) {
        return CompressionFormat::kGzip;
    }

    // Check bzip2
    if (data.size() >= sizeof(kBzip2Magic) &&
        std::memcmp(data.data(), kBzip2Magic, sizeof(kBzip2Magic)) == 0) {
        return CompressionFormat::kBzip2;
    }

    // Check xz
    if (data.size() >= sizeof(kXzMagic) &&
        std::memcmp(data.data(), kXzMagic, sizeof(kXzMagic)) == 0) {
        return CompressionFormat::kXz;
    }

    // Check zstd
    if (data.size() >= sizeof(kZstdMagic) &&
        std::memcmp(data.data(), kZstdMagic, sizeof(kZstdMagic)) == 0) {
        return CompressionFormat::kZstd;
    }

    // Assume uncompressed
    return CompressionFormat::kNone;
}

CompressionFormat detectCompressionFormatFromExtension(const std::filesystem::path& path) {
    auto ext = path.extension().string();

    // Convert to lowercase
    std::transform(
        ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

    if (ext == ".gz" || ext == ".gzip") {
        return CompressionFormat::kGzip;
    }
    if (ext == ".bz2" || ext == ".bzip2") {
        return CompressionFormat::kBzip2;
    }
    if (ext == ".xz" || ext == ".lzma") {
        return CompressionFormat::kXz;
    }
    if (ext == ".zst" || ext == ".zstd") {
        return CompressionFormat::kZstd;
    }

    return CompressionFormat::kNone;
}

std::string_view compressionFormatExtension(CompressionFormat format) {
    switch (format) {
        case CompressionFormat::kGzip:
            return ".gz";
        case CompressionFormat::kBzip2:
            return ".bz2";
        case CompressionFormat::kXz:
            return ".xz";
        case CompressionFormat::kZstd:
            return ".zst";
        case CompressionFormat::kNone:
        case CompressionFormat::kUnknown:
        default:
            return "";
    }
}

std::string_view compressionFormatName(CompressionFormat format) {
    switch (format) {
        case CompressionFormat::kGzip:
            return "gzip";
        case CompressionFormat::kBzip2:
            return "bzip2";
        case CompressionFormat::kXz:
            return "xz";
        case CompressionFormat::kZstd:
            return "zstd";
        case CompressionFormat::kNone:
            return "none";
        case CompressionFormat::kUnknown:
        default:
            return "unknown";
    }
}

bool isCompressionSupported(CompressionFormat format) noexcept {
    switch (format) {
        case CompressionFormat::kNone:
        case CompressionFormat::kGzip:
            return true;
        default:
            return false;
    }
}

std::vector<CompressionFormat> supportedCompressionFormats() {
    return {CompressionFormat::kNone, CompressionFormat::kGzip};
}

// =============================================================================
// GzipStreamBuf Implementation
// =============================================================================

GzipStreamBuf::GzipStreamBuf(std::istream& source, std::size_t bufferSize)
    : source_(&source), inputBuffer_(bufferSize), outputBuffer_(bufferSize) {
    if (bufferSize == 0 || bufferSize > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("gzip buffer size is out of range");
    }
    initZlib();
}

GzipStreamBuf::~GzipStreamBuf() {
    cleanupZlib();
}

GzipStreamBuf::GzipStreamBuf(GzipStreamBuf&& other) noexcept
    : source_(other.source_),
      inputBuffer_(std::move(other.inputBuffer_)),
      outputBuffer_(std::move(other.outputBuffer_)),
      zlibStream_(other.zlibStream_),
      initialized_(other.initialized_),
      streamEnd_(other.streamEnd_) {
    setg(other.eback(), other.gptr(), other.egptr());
    other.setg(nullptr, nullptr, nullptr);
    other.source_ = nullptr;
    other.zlibStream_ = nullptr;
    other.initialized_ = false;
    other.streamEnd_ = false;
}

GzipStreamBuf& GzipStreamBuf::operator=(GzipStreamBuf&& other) noexcept {
    if (this != &other) {
        cleanupZlib();

        source_ = other.source_;
        inputBuffer_ = std::move(other.inputBuffer_);
        outputBuffer_ = std::move(other.outputBuffer_);
        zlibStream_ = other.zlibStream_;
        initialized_ = other.initialized_;
        streamEnd_ = other.streamEnd_;
        setg(other.eback(), other.gptr(), other.egptr());
        other.setg(nullptr, nullptr, nullptr);

        other.source_ = nullptr;
        other.zlibStream_ = nullptr;
        other.initialized_ = false;
        other.streamEnd_ = false;
    }
    return *this;
}

void GzipStreamBuf::initZlib() {
    auto* stream = new z_stream;
    std::memset(stream, 0, sizeof(z_stream));

    // Use inflateInit2 with 16+MAX_WBITS for gzip format
    int ret = inflateInit2(stream, 16 + MAX_WBITS);
    if (ret != Z_OK) {
        delete stream;
        throw std::runtime_error(std::string("failed to initialize zlib: ") + zError(ret));
    }

    zlibStream_ = stream;
    initialized_ = true;
}

void GzipStreamBuf::cleanupZlib() {
    if (zlibStream_) {
        auto* stream = static_cast<z_stream*>(zlibStream_);
        inflateEnd(stream);
        delete stream;
        zlibStream_ = nullptr;
    }
    initialized_ = false;
}

GzipStreamBuf::int_type GzipStreamBuf::underflow() {
    if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }

    if (streamEnd_) {
        return traits_type::eof();
    }

    std::size_t decompressed = decompress();
    if (decompressed == 0) {
        return traits_type::eof();
    }

    setg(outputBuffer_.data(), outputBuffer_.data(), outputBuffer_.data() + decompressed);
    return traits_type::to_int_type(*gptr());
}

std::size_t GzipStreamBuf::decompress() {
    if (!initialized_ || !source_) {
        return 0;
    }

    auto* stream = static_cast<z_stream*>(zlibStream_);
    stream->avail_out = static_cast<uInt>(outputBuffer_.size());
    stream->next_out = reinterpret_cast<Bytef*>(outputBuffer_.data());

    while (stream->avail_out > 0) {
        if (stream->avail_in == 0 && !source_->eof()) {
            source_->read(reinterpret_cast<char*>(inputBuffer_.data()),
                          static_cast<std::streamsize>(inputBuffer_.size()));
            const auto bytesRead = static_cast<std::size_t>(source_->gcount());
            if (bytesRead > 0) {
                stream->avail_in = static_cast<uInt>(bytesRead);
                stream->next_in = inputBuffer_.data();
            }
        }

        const auto availInBefore = stream->avail_in;
        const auto availOutBefore = stream->avail_out;
        const int ret = inflate(stream, Z_NO_FLUSH);

        if (ret == Z_STREAM_END) {
            auto* remainingInput = stream->next_in;
            const auto remainingInputSize = stream->avail_in;
            auto* nextOutput = stream->next_out;
            const auto remainingOutputSize = stream->avail_out;
            const auto resetResult = inflateReset(stream);
            if (resetResult != Z_OK) {
                throw std::runtime_error(
                    std::string("failed to reset zlib for concatenated gzip member: ") +
                    zError(resetResult));
            }
            stream->next_in = remainingInput;
            stream->avail_in = remainingInputSize;
            stream->next_out = nextOutput;
            stream->avail_out = remainingOutputSize;

            if (stream->avail_in == 0 && source_->peek() == std::char_traits<char>::eof()) {
                if (source_->bad()) {
                    throw std::runtime_error("failed while reading the end of a gzip stream");
                }
                streamEnd_ = true;
                break;
            }
            continue;
        }

        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            throw std::runtime_error(std::string("gzip decompression failed: ") + zError(ret));
        }

        const bool producedOutput = stream->avail_out < availOutBefore;
        if (producedOutput) {
            break;
        }

        const bool consumedInput = stream->avail_in < availInBefore;
        const bool inputExhausted = source_->eof() && stream->avail_in == 0;
        if (inputExhausted && !streamEnd_) {
            throw std::runtime_error("gzip decompression failed: truncated or invalid gzip stream");
        }

        if (!consumedInput && ret == Z_BUF_ERROR) {
            break;
        }
    }

    return outputBuffer_.size() - stream->avail_out;
}

// =============================================================================
// CompressedInputStream Implementation
// =============================================================================

CompressedInputStream::CompressedInputStream(const std::filesystem::path& path)
    : std::istream(nullptr) {
    fileStream_ = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!fileStream_->is_open()) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    // Detect format from magic bytes
    std::uint8_t magic[8];
    fileStream_->read(reinterpret_cast<char*>(magic), sizeof(magic));
    auto bytesRead = static_cast<std::size_t>(fileStream_->gcount());

    // Seek back to beginning
    fileStream_->clear();
    fileStream_->seekg(0, std::ios::beg);

    format_ = detectCompressionFormat({magic, bytesRead});

    // Fallback to extension-based detection
    if (format_ == CompressionFormat::kNone && bytesRead < 2) {
        format_ = detectCompressionFormatFromExtension(path);
    }

    setup();
}

CompressedInputStream::CompressedInputStream(std::unique_ptr<std::istream> source,
                                             CompressionFormat format)
    : std::istream(nullptr), sourceStream_(std::move(source)), format_(format) {
    setup();
}

CompressedInputStream::~CompressedInputStream() = default;

void CompressedInputStream::setup() {
    std::istream* source = fileStream_ ? fileStream_.get() : sourceStream_.get();
    if (!source) {
        throw std::runtime_error("no source stream available");
    }

    switch (format_) {
        case CompressionFormat::kNone:
            rdbuf(source->rdbuf());
            break;

        case CompressionFormat::kGzip:
            decompressBuf_ = std::make_unique<GzipStreamBuf>(*source);
            rdbuf(decompressBuf_.get());
            FQC_LOG_DEBUG("Opened gzip compressed stream");
            break;

        default:
            throw std::runtime_error(std::string("unsupported compression format: ") +
                                     std::string(compressionFormatName(format_)));
    }
}

// =============================================================================
// Factory Functions
// =============================================================================

auto openCompressedFile(const std::filesystem::path& path)
    -> Result<std::unique_ptr<std::istream>> {
    try {
        return std::unique_ptr<std::istream>(std::make_unique<CompressedInputStream>(path));
    } catch (const std::exception& e) {
        return makeError<std::unique_ptr<std::istream>>(ErrorCode::kIOError, e.what());
    }
}

/// @brief A streambuf that prepends buffered bytes before delegating to another streambuf.
/// @note Enables streaming stdin without loading the entire input into memory.
class PrependStreamBuf : public std::streambuf {
public:
    PrependStreamBuf(const std::uint8_t* prefix, std::size_t prefixLen, std::streambuf* underlying)
        : prefix_(reinterpret_cast<const char*>(prefix),
                  reinterpret_cast<const char*>(prefix) + prefixLen),
          underlying_(underlying),
          phase_(prefixLen > 0 ? Phase::kPrefix : Phase::kUnderlying) {
        if (phase_ == Phase::kPrefix) {
            setg(prefix_.data(),
                 prefix_.data(),
                 prefix_.data() + static_cast<std::ptrdiff_t>(prefix_.size()));
        }
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (phase_ == Phase::kPrefix) {
            phase_ = Phase::kUnderlying;
        }
        if (phase_ == Phase::kUnderlying && underlying_) {
            return underlying_->sgetc();
        }
        return traits_type::eof();
    }

    int_type uflow() override {
        if (gptr() < egptr()) {
            char_type ch = *gptr();
            gbump(1);
            return traits_type::to_int_type(ch);
        }
        if (phase_ == Phase::kPrefix) {
            phase_ = Phase::kUnderlying;
        }
        if (phase_ == Phase::kUnderlying && underlying_) {
            return underlying_->sbumpc();
        }
        return traits_type::eof();
    }

    std::streamsize xsgetn(char_type* s, std::streamsize count) override {
        std::streamsize totalRead = 0;
        // Drain prefix first
        if (phase_ == Phase::kPrefix) {
            std::streamsize avail = egptr() - gptr();
            std::streamsize toCopy = std::min(count, avail);
            std::memcpy(s, gptr(), static_cast<std::size_t>(toCopy));
            gbump(static_cast<int>(toCopy));
            totalRead += toCopy;
            s += toCopy;
            count -= toCopy;
            if (gptr() >= egptr()) {
                phase_ = Phase::kUnderlying;
            }
        }
        // Read from underlying
        if (count > 0 && phase_ == Phase::kUnderlying && underlying_) {
            totalRead += underlying_->sgetn(s, count);
        }
        return totalRead;
    }

private:
    enum class Phase : std::uint8_t { kPrefix, kUnderlying };
    std::vector<char> prefix_;
    std::streambuf* underlying_;
    Phase phase_;
};

/// @brief An istream that owns a PrependStreamBuf for streaming stdin.
class PrependInputStream : public std::istream {
public:
    PrependInputStream(const std::uint8_t* prefix,
                       std::size_t prefixLen,
                       std::streambuf* underlying)
        : std::istream(nullptr), buf_(prefix, prefixLen, underlying) {
        rdbuf(&buf_);
    }

private:
    PrependStreamBuf buf_;
};

auto openInputFile(const std::filesystem::path& path) -> Result<std::unique_ptr<std::istream>> {
    if (path == "-") {
        FQC_LOG_DEBUG("Opening stdin for input");

        std::uint8_t magic[8];
        std::cin.read(reinterpret_cast<char*>(magic), sizeof(magic));
        auto bytesRead = static_cast<std::size_t>(std::cin.gcount());

        auto format = detectCompressionFormat({magic, bytesRead});

        if (format == CompressionFormat::kNone) {
            return std::unique_ptr<std::istream>(
                std::make_unique<PrependInputStream>(magic, bytesRead, std::cin.rdbuf()));
        }

        if (!isCompressionSupported(format)) {
            return makeError<std::unique_ptr<std::istream>>(
                ErrorCode::kIOError,
                "compressed stdin not supported for format: " +
                    std::string(compressionFormatName(format)));
        }

        try {
            auto prefixed =
                std::make_unique<PrependInputStream>(magic, bytesRead, std::cin.rdbuf());
            return std::unique_ptr<std::istream>(
                std::make_unique<CompressedInputStream>(std::move(prefixed), format));
        } catch (const std::exception& e) {
            return makeError<std::unique_ptr<std::istream>>(ErrorCode::kIOError, e.what());
        }
    }

    return openCompressedFile(path);
}

}  // namespace fqc::io
