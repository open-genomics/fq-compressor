// =============================================================================
// fq-compressor - FQC v2 Sequential Archive
// =============================================================================

#include "fqc/format/archive.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <istream>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include <xxhash.h>
#include <zstd.h>

namespace fqc::format {

namespace {

constexpr std::array<std::uint8_t, 8> kArchiveMagic = {'F', 'Q', 'C', 'V', '2', '\r', '\n', 0x1A};
constexpr std::uint32_t kFrameMagic = 0x324D5246U;
constexpr std::uint32_t kFooterMagic = 0x32444E45U;
constexpr std::uint16_t kGlobalHeaderSize = 32;
constexpr std::uint32_t kFrameHeaderSize = 72;
constexpr std::uint32_t kFooterSize = 40;
constexpr std::uint32_t kFlagPaired = 1U << 0U;
constexpr std::uint8_t kIdCodecZstd = 1;
constexpr std::uint8_t kSequenceCodecPackedZstd = 1;
constexpr std::uint8_t kQualityCodecZstd = 1;
constexpr std::size_t kCodecMemoryReserve = std::size_t{16} * 1024 * 1024;
// ID/sequence streams always compress at this level (throughput-oriented; the
// ratio/level curve flattens early on these streams). Only the quality stream
// gets a configurable level (ArchiveOptions::qualityZstdLevel, stage I).
constexpr int kStreamZstdLevel = 1;
// Accepted range for the configurable quality level (matches the CLI check).
constexpr int kMinZstdLevel = 1;
constexpr int kMaxZstdLevel = 19;

using Bytes = std::vector<std::uint8_t>;

void appendU16(Bytes& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendU32(Bytes& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void appendU64(Bytes& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendVarint(Bytes& output, std::uint64_t value) {
    while (value >= 0x80U) {
        output.push_back(static_cast<std::uint8_t>(value) | 0x80U);
        value >>= 7U;
    }
    output.push_back(static_cast<std::uint8_t>(value));
}

[[nodiscard]] constexpr auto varintSize(std::uint64_t value) noexcept -> std::size_t {
    std::size_t result = 1;
    while (value >= 0x80U) {
        value >>= 7U;
        ++result;
    }
    return result;
}

[[nodiscard]] auto checkedAdd(std::size_t& destination, std::size_t value) noexcept -> bool {
    if (value > std::numeric_limits<std::size_t>::max() - destination) {
        return false;
    }
    destination += value;
    return true;
}

void appendString(Bytes& output, std::string_view value) {
    appendVarint(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] auto checkedU32(std::size_t value, std::string_view field) -> Result<std::uint32_t> {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return makeError<std::uint32_t>(ErrorCode::kUsageError,
                                        std::string(field) + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] auto writeBytes(std::ostream& output,
                              std::span<const std::uint8_t> bytes) -> VoidResult {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        return makeVoidError(ErrorCode::kIOError, "failed to write FQC v2 archive");
    }
    return {};
}

[[nodiscard]] auto readExact(std::istream& input, std::span<std::uint8_t> bytes) -> VoidResult {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return makeVoidError(ErrorCode::kFormatError, "truncated FQC v2 archive");
    }
    return {};
}

class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] auto readU8() -> Result<std::uint8_t> {
        if (offset_ >= data_.size()) {
            return makeError<std::uint8_t>(ErrorCode::kFormatError, "truncated FQC v2 field");
        }
        return data_[offset_++];
    }

    [[nodiscard]] auto readU16() -> Result<std::uint16_t> {
        auto bytes = readBytes(2);
        if (!bytes) {
            return makeError<std::uint16_t>(bytes.error());
        }
        return static_cast<std::uint16_t>((*bytes)[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>((*bytes)[1]) << 8U);
    }

    [[nodiscard]] auto readU32() -> Result<std::uint32_t> {
        auto bytes = readBytes(4);
        if (!bytes) {
            return makeError<std::uint32_t>(bytes.error());
        }
        return static_cast<std::uint32_t>((*bytes)[0]) |
            (static_cast<std::uint32_t>((*bytes)[1]) << 8U) |
            (static_cast<std::uint32_t>((*bytes)[2]) << 16U) |
            (static_cast<std::uint32_t>((*bytes)[3]) << 24U);
    }

    [[nodiscard]] auto readU64() -> Result<std::uint64_t> {
        auto bytes = readBytes(8);
        if (!bytes) {
            return makeError<std::uint64_t>(bytes.error());
        }
        std::uint64_t result = 0;
        for (unsigned index = 0; index < 8; ++index) {
            result |= static_cast<std::uint64_t>((*bytes)[index]) << (index * 8U);
        }
        return result;
    }

    [[nodiscard]] auto readVarint() -> Result<std::uint64_t> {
        std::uint64_t result = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            auto byte = readU8();
            if (!byte) {
                return makeError<std::uint64_t>(byte.error());
            }
            if (shift == 63 && (*byte & 0xFEU) != 0U) {
                return makeError<std::uint64_t>(ErrorCode::kFormatError, "FQC v2 varint overflow");
            }
            result |= static_cast<std::uint64_t>(*byte & 0x7FU) << shift;
            if ((*byte & 0x80U) == 0U) {
                return result;
            }
        }
        return makeError<std::uint64_t>(ErrorCode::kFormatError, "FQC v2 varint overflow");
    }

    [[nodiscard]] auto readBytes(std::size_t size) -> Result<std::span<const std::uint8_t>> {
        if (size > data_.size() - offset_) {
            return makeError<std::span<const std::uint8_t>>(ErrorCode::kFormatError,
                                                            "truncated FQC v2 field");
        }
        auto result = data_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return offset_ == data_.size();
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
};

[[nodiscard]] auto readString(Cursor& cursor) -> Result<std::string> {
    auto size = cursor.readVarint();
    if (!size) {
        return makeError<std::string>(size.error());
    }
    if (*size > std::numeric_limits<std::size_t>::max()) {
        return makeError<std::string>(ErrorCode::kFormatError, "FQC v2 string is too large");
    }
    auto bytes = cursor.readBytes(static_cast<std::size_t>(*size));
    if (!bytes) {
        return makeError<std::string>(bytes.error());
    }
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

[[nodiscard]] auto baseCode(char base, bool& exceptional) noexcept -> std::uint8_t {
    exceptional = false;
    switch (base) {
        case 'A':
            return 0;
        case 'C':
            return 1;
        case 'G':
            return 2;
        case 'T':
            return 3;
        default:
            exceptional = true;
            return 0;
    }
}

[[nodiscard]] auto codeBase(std::uint8_t code) noexcept -> char {
    constexpr std::array<char, 4> kBases = {'A', 'C', 'G', 'T'};
    return kBases[code & 0x03U];
}

[[nodiscard]] constexpr auto packedSequenceSize(std::size_t sequenceSize) noexcept -> std::size_t {
    return sequenceSize / 4 + (sequenceSize % 4 == 0 ? 0 : 1);
}

[[nodiscard]] constexpr auto validIupacBase(char base) noexcept -> bool {
    constexpr std::string_view kIupacBases = "ACGTRYSWKMBDHVNacgtryswkmbdhvn";
    return kIupacBases.contains(base);
}

[[nodiscard]] constexpr auto validQuality(char quality) noexcept -> bool {
    return quality >= '!' && quality <= '~';
}

[[nodiscard]] auto validRecordShape(const ReadRecord& record) noexcept -> bool {
    return record.isValid() && !record.id.contains(' ') && !record.id.contains('\r') &&
        !record.id.contains('\n') && !record.comment.contains('\r') &&
        !record.comment.contains('\n');
}

void appendPackedSequence(Bytes& output, std::string_view sequence) {
    appendVarint(output, sequence.size());
    const auto packedSize = packedSequenceSize(sequence.size());
    const auto packedStart = output.size();
    output.resize(output.size() + packedSize, 0);

    std::size_t exceptionCount = 0;
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        bool exceptional = false;
        const auto code = baseCode(sequence[index], exceptional);
        const auto shift = static_cast<unsigned>((index % 4) * 2);
        output[packedStart + index / 4] |= static_cast<std::uint8_t>(code << shift);
        if (exceptional) {
            ++exceptionCount;
        }
    }

    appendVarint(output, exceptionCount);
    if (exceptionCount == 0) {
        return;
    }
    std::size_t previous = 0;
    std::size_t emitted = 0;
    for (std::size_t position = 0; position < sequence.size(); ++position) {
        bool exceptional = false;
        (void)baseCode(sequence[position], exceptional);
        if (exceptional) {
            appendVarint(output, emitted == 0 ? position : position - previous);
            output.push_back(static_cast<std::uint8_t>(sequence[position]));
            previous = position;
            ++emitted;
        }
    }
}

[[nodiscard]] auto readPackedSequence(Cursor& cursor) -> Result<std::string> {
    auto sequenceSizeValue = cursor.readVarint();
    if (!sequenceSizeValue || *sequenceSizeValue > std::numeric_limits<std::size_t>::max()) {
        return makeError<std::string>(ErrorCode::kFormatError, "invalid FQC v2 sequence length");
    }
    const auto sequenceSize = static_cast<std::size_t>(*sequenceSizeValue);
    auto packed = cursor.readBytes(packedSequenceSize(sequenceSize));
    if (!packed) {
        return makeError<std::string>(packed.error());
    }

    std::string sequence(sequenceSize, 'A');
    for (std::size_t index = 0; index < sequenceSize; ++index) {
        const auto shift = static_cast<unsigned>((index % 4) * 2);
        sequence[index] = codeBase(static_cast<std::uint8_t>((*packed)[index / 4] >> shift));
    }

    auto exceptionCount = cursor.readVarint();
    if (!exceptionCount || *exceptionCount > sequenceSize) {
        return makeError<std::string>(ErrorCode::kFormatError,
                                      "invalid FQC v2 sequence exceptions");
    }
    std::size_t position = 0;
    for (std::uint64_t index = 0; index < *exceptionCount; ++index) {
        auto delta = cursor.readVarint();
        auto base = cursor.readU8();
        if (!delta || !base || *delta > std::numeric_limits<std::size_t>::max()) {
            return makeError<std::string>(ErrorCode::kFormatError,
                                          "invalid FQC v2 sequence exception");
        }
        const auto narrowedDelta = static_cast<std::size_t>(*delta);
        if (index > 0 && narrowedDelta == 0) {
            return makeError<std::string>(ErrorCode::kFormatError,
                                          "duplicate FQC v2 sequence exception");
        }
        if (narrowedDelta > sequenceSize - position) {
            return makeError<std::string>(ErrorCode::kFormatError,
                                          "FQC v2 sequence exception is out of range");
        }
        position += narrowedDelta;
        if (position >= sequenceSize) {
            return makeError<std::string>(ErrorCode::kFormatError,
                                          "FQC v2 sequence exception is out of range");
        }
        const auto decodedBase = static_cast<char>(*base);
        if (!validIupacBase(decodedBase)) {
            return makeError<std::string>(ErrorCode::kFormatError,
                                          "invalid FQC v2 sequence exception base");
        }
        sequence[position] = decodedBase;
    }
    return sequence;
}

[[nodiscard]] auto compress(std::span<const std::uint8_t> input, int level) -> Result<Bytes> {
    Bytes output(ZSTD_compressBound(input.size()));
    const auto result =
        ZSTD_compress(output.data(), output.size(), input.data(), input.size(), level);
    if (ZSTD_isError(result) != 0U) {
        return makeError<Bytes>(ErrorCode::kInternalError,
                                std::string("Zstd compression failed: ") +
                                    ZSTD_getErrorName(result));
    }
    output.resize(result);
    return output;
}

[[nodiscard]] auto decompress(std::span<const std::uint8_t> input,
                              std::size_t outputSize) -> Result<Bytes> {
    Bytes output(outputSize);
    const auto result = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(result) != 0U || result != outputSize) {
        return makeError<Bytes>(ErrorCode::kFormatError, "FQC v2 Zstd stream failed to decompress");
    }
    return output;
}

[[nodiscard]] auto logicalChecksum(std::span<const std::uint8_t> ids,
                                   std::span<const std::uint8_t> sequences,
                                   std::span<const std::uint8_t> qualities) -> std::uint64_t {
    auto checksum = XXH64(ids.data(), ids.size(), 0);
    checksum = XXH64(sequences.data(), sequences.size(), checksum);
    return XXH64(qualities.data(), qualities.size(), checksum);
}

[[nodiscard]] auto advanceGlobalChecksum(std::uint64_t current,
                                         std::uint64_t frameChecksum) -> std::uint64_t {
    Bytes encoded;
    encoded.reserve(8);
    appendU64(encoded, frameChecksum);
    return XXH64(encoded.data(), encoded.size(), current);
}

struct RawStreams {
    Bytes ids;
    Bytes sequences;
    Bytes qualities;
    std::uint64_t totalBases = 0;
};

struct RawStreamSizes {
    std::size_t ids = 0;
    std::size_t sequences = 0;
    std::size_t qualities = 0;
    std::uint64_t totalBases = 0;
};

[[nodiscard]] auto measureRawStreams(std::span<const ReadRecord> records)
    -> Result<RawStreamSizes> {
    RawStreamSizes sizes{
        .ids = varintSize(records.size()),
        .sequences = varintSize(records.size()),
        .qualities = varintSize(records.size()),
    };
    for (const auto& record : records) {
        if (!validRecordShape(record)) {
            return makeError<RawStreamSizes>(ErrorCode::kUsageError,
                                             "invalid logical FASTQ record");
        }
        if (!checkedAdd(sizes.ids, varintSize(record.id.size())) ||
            !checkedAdd(sizes.ids, record.id.size()) ||
            !checkedAdd(sizes.ids, varintSize(record.comment.size())) ||
            !checkedAdd(sizes.ids, record.comment.size()) ||
            !checkedAdd(sizes.qualities, varintSize(record.quality.size())) ||
            !checkedAdd(sizes.qualities, record.quality.size()) ||
            !checkedAdd(sizes.sequences, varintSize(record.sequence.size())) ||
            !checkedAdd(sizes.sequences, packedSequenceSize(record.sequence.size()))) {
            return makeError<RawStreamSizes>(ErrorCode::kUsageError, "FQC v2 frame size overflow");
        }

        std::size_t exceptionCount = 0;
        std::size_t previous = 0;
        for (std::size_t position = 0; position < record.sequence.size(); ++position) {
            if (!validIupacBase(record.sequence[position])) {
                return makeError<RawStreamSizes>(ErrorCode::kUsageError,
                                                 "invalid logical FASTQ record");
            }
            bool exceptional = false;
            (void)baseCode(record.sequence[position], exceptional);
            if (exceptional) {
                const auto delta = exceptionCount == 0 ? position : position - previous;
                if (!checkedAdd(sizes.sequences, varintSize(delta) + 1)) {
                    return makeError<RawStreamSizes>(ErrorCode::kUsageError,
                                                     "FQC v2 frame size overflow");
                }
                previous = position;
                ++exceptionCount;
            }
        }
        if (!std::ranges::all_of(record.quality, validQuality)) {
            return makeError<RawStreamSizes>(ErrorCode::kUsageError,
                                             "invalid logical FASTQ record");
        }
        if (!checkedAdd(sizes.sequences, varintSize(exceptionCount)) ||
            record.sequence.size() > std::numeric_limits<std::uint64_t>::max() - sizes.totalBases) {
            return makeError<RawStreamSizes>(ErrorCode::kUsageError, "FQC v2 frame size overflow");
        }
        sizes.totalBases += record.sequence.size();
    }
    return sizes;
}

[[nodiscard]] auto estimateCompressionPeak(std::span<const ReadRecord> records,
                                           const RawStreamSizes& sizes) -> Result<std::size_t> {
    std::size_t peak = kCodecMemoryReserve;
    if (records.size() > std::numeric_limits<std::size_t>::max() / (sizeof(ReadRecord) * 2) ||
        !checkedAdd(peak, records.size() * sizeof(ReadRecord) * 2)) {
        return makeError<std::size_t>(ErrorCode::kUsageError,
                                      "FQC v2 frame memory estimate overflow");
    }
    for (const auto& record : records) {
        for (const auto capacity : {record.id.capacity(),
                                    record.comment.capacity(),
                                    record.sequence.capacity(),
                                    record.quality.capacity()}) {
            if (capacity == std::numeric_limits<std::size_t>::max() ||
                !checkedAdd(peak, capacity + 1)) {
                return makeError<std::size_t>(ErrorCode::kUsageError,
                                              "FQC v2 frame memory estimate overflow");
            }
        }
    }
    for (const auto rawSize : {sizes.ids, sizes.sequences, sizes.qualities}) {
        const auto compressedBound = ZSTD_compressBound(rawSize);
        if (!checkedAdd(peak, rawSize) || !checkedAdd(peak, compressedBound)) {
            return makeError<std::size_t>(ErrorCode::kUsageError,
                                          "FQC v2 frame memory estimate overflow");
        }
    }
    return peak;
}

[[nodiscard]] auto encodeRawStreams(std::span<const ReadRecord> records,
                                    const RawStreamSizes& sizes) -> RawStreams {
    RawStreams streams;
    streams.ids.reserve(sizes.ids);
    streams.sequences.reserve(sizes.sequences);
    streams.qualities.reserve(sizes.qualities);
    appendVarint(streams.ids, records.size());
    appendVarint(streams.sequences, records.size());
    appendVarint(streams.qualities, records.size());
    for (const auto& record : records) {
        appendString(streams.ids, record.id);
        appendString(streams.ids, record.comment);
        appendPackedSequence(streams.sequences, record.sequence);
        appendString(streams.qualities, record.quality);
    }
    streams.totalBases = sizes.totalBases;
    return streams;
}

[[nodiscard]] auto decodeRawStreams(std::span<const std::uint8_t> ids,
                                    std::span<const std::uint8_t> sequences,
                                    std::span<const std::uint8_t> qualities,
                                    std::uint32_t expectedCount)
    -> Result<std::vector<ReadRecord>> {
    Cursor idCursor(ids);
    Cursor sequenceCursor(sequences);
    Cursor qualityCursor(qualities);
    auto idCount = idCursor.readVarint();
    auto sequenceCount = sequenceCursor.readVarint();
    auto qualityCount = qualityCursor.readVarint();
    if (!idCount || !sequenceCount || !qualityCount || *idCount != expectedCount ||
        *sequenceCount != expectedCount || *qualityCount != expectedCount) {
        return makeError<std::vector<ReadRecord>>(ErrorCode::kFormatError,
                                                  "FQC v2 stream counts disagree");
    }

    std::vector<ReadRecord> records;
    records.reserve(expectedCount);
    for (std::uint32_t index = 0; index < expectedCount; ++index) {
        auto id = readString(idCursor);
        auto comment = readString(idCursor);
        auto sequence = readPackedSequence(sequenceCursor);
        auto quality = readString(qualityCursor);
        if (!id || !comment || !sequence || !quality) {
            return makeError<std::vector<ReadRecord>>(ErrorCode::kFormatError,
                                                      "invalid FQC v2 record stream");
        }
        if (sequence->size() != quality->size()) {
            return makeError<std::vector<ReadRecord>>(ErrorCode::kFormatError,
                                                      "FQC v2 sequence/quality length mismatch");
        }
        ReadRecord record(
            std::move(*id), std::move(*comment), std::move(*sequence), std::move(*quality));
        if (!validRecordShape(record) || !std::ranges::all_of(record.quality, validQuality)) {
            return makeError<std::vector<ReadRecord>>(ErrorCode::kFormatError,
                                                      "invalid logical FASTQ record in archive");
        }
        records.push_back(std::move(record));
    }

    if (!idCursor.empty() || !sequenceCursor.empty() || !qualityCursor.empty()) {
        return makeError<std::vector<ReadRecord>>(ErrorCode::kFormatError,
                                                  "trailing bytes in FQC v2 record stream");
    }
    return records;
}

[[nodiscard]] auto validProfile(std::uint8_t value) noexcept -> bool {
    return value >= static_cast<std::uint8_t>(DatasetProfile::kIllumina) &&
        value <= static_cast<std::uint8_t>(DatasetProfile::kPacBioClr);
}

[[nodiscard]] auto estimateDecompressionPeak(std::size_t rawIds,
                                             std::size_t ids,
                                             std::size_t rawSequences,
                                             std::size_t sequences,
                                             std::size_t rawQualities,
                                             std::size_t qualities,
                                             std::size_t recordCount) -> Result<std::size_t> {
    std::size_t peak = kCodecMemoryReserve;
    if (recordCount > std::numeric_limits<std::size_t>::max() / (sizeof(ReadRecord) * 2) ||
        !checkedAdd(peak, recordCount * sizeof(ReadRecord) * 2)) {
        return makeError<std::size_t>(ErrorCode::kFormatError,
                                      "FQC v2 frame memory estimate overflow");
    }
    for (const auto rawSize : {rawIds, rawSequences, rawQualities}) {
        if (!checkedAdd(peak, rawSize) || !checkedAdd(peak, rawSize)) {
            return makeError<std::size_t>(ErrorCode::kFormatError,
                                          "FQC v2 frame memory estimate overflow");
        }
    }
    for (const auto encodedSize : {ids, sequences, qualities}) {
        if (!checkedAdd(peak, encodedSize)) {
            return makeError<std::size_t>(ErrorCode::kFormatError,
                                          "FQC v2 frame memory estimate overflow");
        }
    }
    return peak;
}

}  // namespace

auto parseProfile(std::string_view value) -> Result<DatasetProfile> {
    if (value == "illumina") {
        return DatasetProfile::kIllumina;
    }
    if (value == "ont") {
        return DatasetProfile::kOnt;
    }
    if (value == "pacbio-hifi") {
        return DatasetProfile::kPacBioHiFi;
    }
    if (value == "pacbio-clr") {
        return DatasetProfile::kPacBioClr;
    }
    return makeError<DatasetProfile>(ErrorCode::kUsageError,
                                     "unknown dataset profile: " + std::string(value));
}

auto encodeFrame(std::span<const ReadRecord> records,
                 const ArchiveOptions& options) -> Result<std::unique_ptr<EncodedFrame>> {
    if (records.empty()) {
        return makeError<std::unique_ptr<EncodedFrame>>(ErrorCode::kUsageError,
                                                        "FQC v2 frame cannot be empty");
    }
    if (options.paired && records.size() % 2 != 0) {
        return makeError<std::unique_ptr<EncodedFrame>>(
            ErrorCode::kUsageError, "paired FQC v2 frame must contain complete pairs");
    }
    auto recordCount = checkedU32(records.size(), "frame record count");
    if (!recordCount) {
        return makeError<std::unique_ptr<EncodedFrame>>(recordCount.error().code,
                                                        recordCount.error().message);
    }
    auto rawSizes = measureRawStreams(records);
    if (!rawSizes) {
        return makeError<std::unique_ptr<EncodedFrame>>(rawSizes.error().code,
                                                        rawSizes.error().message);
    }
    if (rawSizes->ids > options.maxFrameBytes || rawSizes->sequences > options.maxFrameBytes ||
        rawSizes->qualities > options.maxFrameBytes) {
        return makeError<std::unique_ptr<EncodedFrame>>(
            ErrorCode::kUsageError, "FQC v2 frame exceeds configured raw stream limit");
    }
    auto peakMemory = estimateCompressionPeak(records, *rawSizes);
    if (!peakMemory) {
        return makeError<std::unique_ptr<EncodedFrame>>(peakMemory.error().code,
                                                        peakMemory.error().message);
    }
    if (*peakMemory > options.memoryLimitBytes) {
        return makeError<std::unique_ptr<EncodedFrame>>(
            ErrorCode::kUsageError, "FQC v2 frame exceeds configured memory limit");
    }
    auto raw = encodeRawStreams(records, *rawSizes);
    auto frame = std::make_unique<EncodedFrame>();
    frame->rawIds = std::move(raw.ids);
    frame->rawSequences = std::move(raw.sequences);
    frame->rawQualities = std::move(raw.qualities);
    frame->recordCount = *recordCount;
    frame->totalBases = raw.totalBases;
    frame->checksum = logicalChecksum(frame->rawIds, frame->rawSequences, frame->rawQualities);
    return frame;
}

auto compressFrame(std::unique_ptr<EncodedFrame> frame,
                   int qualityZstdLevel) -> Result<std::unique_ptr<CompressedFrame>> {
    if (frame == nullptr) {
        return makeError<std::unique_ptr<CompressedFrame>>(ErrorCode::kUsageError,
                                                           "FQC v2 encoded frame is null");
    }
    auto compressed = std::make_unique<CompressedFrame>();
    compressed->rawIdsSize = frame->rawIds.size();
    compressed->rawSequencesSize = frame->rawSequences.size();
    compressed->rawQualitiesSize = frame->rawQualities.size();
    compressed->recordCount = frame->recordCount;
    compressed->totalBases = frame->totalBases;
    compressed->checksum = frame->checksum;

    // One stream at a time, releasing the raw stream immediately afterwards:
    // the resident peak stays at raw x3 + one ZSTD_compressBound scratch
    // (already covered per-frame by estimateCompressionPeak) instead of
    // raw x3 + bound x3. ID/sequence stay at kStreamZstdLevel; only quality
    // uses the caller-configured level (stage I).
    auto ids = compress(frame->rawIds, kStreamZstdLevel);
    if (!ids) {
        return makeError<std::unique_ptr<CompressedFrame>>(ids.error().code, ids.error().message);
    }
    compressed->ids = std::move(*ids);
    Bytes().swap(frame->rawIds);

    auto sequences = compress(frame->rawSequences, kStreamZstdLevel);
    if (!sequences) {
        return makeError<std::unique_ptr<CompressedFrame>>(sequences.error().code,
                                                           sequences.error().message);
    }
    compressed->sequences = std::move(*sequences);
    Bytes().swap(frame->rawSequences);

    auto qualities = compress(frame->rawQualities, qualityZstdLevel);
    if (!qualities) {
        return makeError<std::unique_ptr<CompressedFrame>>(qualities.error().code,
                                                           qualities.error().message);
    }
    compressed->qualities = std::move(*qualities);
    Bytes().swap(frame->rawQualities);

    return compressed;
}

// =============================================================================
// ArchiveWriter
// =============================================================================

ArchiveWriter::ArchiveWriter(std::ostream& output, ArchiveOptions options)
    : output_(output),
      options_(options),
      metadata_{.version = kArchiveVersion, .profile = options.profile, .paired = options.paired} {}

auto ArchiveWriter::ensureHeader() -> VoidResult {
    if (headerWritten_) {
        return {};
    }
    if (options_.maxFrameBytes == 0 || options_.memoryLimitBytes <= kCodecMemoryReserve) {
        return makeVoidError(ErrorCode::kUsageError,
                             "FQC v2 frame and memory limits must be positive");
    }
    if (options_.qualityZstdLevel < kMinZstdLevel || options_.qualityZstdLevel > kMaxZstdLevel) {
        return makeVoidError(ErrorCode::kUsageError, "FQC v2 quality zstd level out of range");
    }
    if (!validProfile(static_cast<std::uint8_t>(metadata_.profile))) {
        return makeVoidError(ErrorCode::kUsageError, "invalid FQC v2 dataset profile");
    }

    Bytes header;
    header.reserve(kGlobalHeaderSize);
    header.insert(header.end(), kArchiveMagic.begin(), kArchiveMagic.end());
    appendU16(header, kArchiveVersion);
    appendU16(header, kGlobalHeaderSize);
    appendU32(header, metadata_.paired ? kFlagPaired : 0U);
    header.push_back(static_cast<std::uint8_t>(metadata_.profile));
    header.push_back(kIdCodecZstd);
    header.push_back(kSequenceCodecPackedZstd);
    header.push_back(kQualityCodecZstd);
    appendU32(header, 0);
    appendU64(header, XXH64(header.data(), header.size(), 0));
    auto result = writeBytes(output_, header);
    if (result) {
        headerWritten_ = true;
        stats_.encodedBytes += header.size();
    }
    return result;
}

auto ArchiveWriter::writeFrame(std::span<const ReadRecord> records) -> VoidResult {
    if (finished_) {
        return makeVoidError(ErrorCode::kFormatError, "FQC v2 writer is already finished");
    }
    auto encoded = encodeFrame(records, options_);
    if (!encoded) {
        return makeVoidError(encoded.error().code, encoded.error().message);
    }
    auto compressed = compressFrame(std::move(*encoded), options_.qualityZstdLevel);
    if (!compressed) {
        return makeVoidError(compressed.error().code, compressed.error().message);
    }
    return writeCompressedFrame(std::move(*compressed));
}

auto ArchiveWriter::writeCompressedFrame(std::unique_ptr<CompressedFrame> frame) -> VoidResult {
    if (finished_) {
        return makeVoidError(ErrorCode::kFormatError, "FQC v2 writer is already finished");
    }
    if (frame == nullptr) {
        return makeVoidError(ErrorCode::kUsageError, "FQC v2 compressed frame is null");
    }
    if (auto headerResult = ensureHeader(); !headerResult) {
        return headerResult;
    }

    Bytes frameHeader;
    frameHeader.reserve(kFrameHeaderSize);
    appendU32(frameHeader, kFrameMagic);
    appendU32(frameHeader, kFrameHeaderSize);
    appendU32(frameHeader, static_cast<std::uint32_t>(stats_.frameCount));
    appendU32(frameHeader, frame->recordCount);
    appendU64(frameHeader, frame->rawIdsSize);
    appendU64(frameHeader, frame->ids.size());
    appendU64(frameHeader, frame->rawSequencesSize);
    appendU64(frameHeader, frame->sequences.size());
    appendU64(frameHeader, frame->rawQualitiesSize);
    appendU64(frameHeader, frame->qualities.size());
    appendU64(frameHeader, frame->checksum);

    for (const auto bytes : {std::span<const std::uint8_t>(frameHeader),
                             std::span<const std::uint8_t>(frame->ids),
                             std::span<const std::uint8_t>(frame->sequences),
                             std::span<const std::uint8_t>(frame->qualities)}) {
        if (auto result = writeBytes(output_, bytes); !result) {
            return result;
        }
    }

    ++stats_.frameCount;
    stats_.recordCount += frame->recordCount;
    stats_.totalBases += frame->totalBases;
    stats_.encodedBytes +=
        frameHeader.size() + frame->ids.size() + frame->sequences.size() + frame->qualities.size();
    globalChecksum_ = advanceGlobalChecksum(globalChecksum_, frame->checksum);
    return {};
}

auto ArchiveWriter::finish() -> VoidResult {
    if (finished_) {
        return {};
    }
    if (auto headerResult = ensureHeader(); !headerResult) {
        return headerResult;
    }
    Bytes footer;
    footer.reserve(kFooterSize);
    appendU32(footer, kFooterMagic);
    appendU32(footer, kFooterSize);
    appendU64(footer, stats_.frameCount);
    appendU64(footer, stats_.recordCount);
    appendU64(footer, stats_.totalBases);
    appendU64(footer, globalChecksum_);
    auto result = writeBytes(output_, footer);
    if (result) {
        output_.flush();
        if (!output_) {
            return makeVoidError(ErrorCode::kIOError, "failed to flush FQC v2 archive");
        }
        stats_.encodedBytes += footer.size();
        finished_ = true;
    }
    return result;
}

// =============================================================================
// ArchiveReader
// =============================================================================

ArchiveReader::ArchiveReader(std::istream& input,
                             std::size_t maxFrameBytes,
                             std::size_t memoryLimitBytes)
    : input_(input), maxFrameBytes_(maxFrameBytes), memoryLimitBytes_(memoryLimitBytes) {}

auto ArchiveReader::open() -> Result<ArchiveMetadata> {
    if (opened_) {
        return metadata_;
    }
    if (maxFrameBytes_ == 0 || memoryLimitBytes_ <= kCodecMemoryReserve) {
        return makeError<ArchiveMetadata>(ErrorCode::kUsageError,
                                          "FQC v2 frame and memory limits must be positive");
    }
    std::array<std::uint8_t, kGlobalHeaderSize> header{};
    if (auto result = readExact(input_, header); !result) {
        return makeError<ArchiveMetadata>(result.error());
    }
    if (!std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), header.begin())) {
        return makeError<ArchiveMetadata>(ErrorCode::kFormatError, "not an FQC v2 archive");
    }
    Cursor cursor(header);
    auto magic = cursor.readBytes(kArchiveMagic.size());
    auto version = cursor.readU16();
    auto headerSize = cursor.readU16();
    auto flags = cursor.readU32();
    auto profile = cursor.readU8();
    auto idCodec = cursor.readU8();
    auto sequenceCodec = cursor.readU8();
    auto qualityCodec = cursor.readU8();
    auto reserved = cursor.readU32();
    auto storedChecksum = cursor.readU64();
    (void)magic;
    if (!version || !headerSize || !flags || !profile || !idCodec || !sequenceCodec ||
        !qualityCodec || !reserved || !storedChecksum) {
        return makeError<ArchiveMetadata>(ErrorCode::kFormatError, "invalid FQC v2 global header");
    }
    if (*version != kArchiveVersion || *headerSize != kGlobalHeaderSize) {
        return makeError<ArchiveMetadata>(ErrorCode::kUnsupportedCodec,
                                          "unsupported FQC archive version");
    }
    if ((*flags & ~kFlagPaired) != 0U || *reserved != 0U || !validProfile(*profile) ||
        *idCodec != kIdCodecZstd || *sequenceCodec != kSequenceCodecPackedZstd ||
        *qualityCodec != kQualityCodecZstd) {
        return makeError<ArchiveMetadata>(ErrorCode::kUnsupportedCodec,
                                          "unsupported FQC v2 header fields");
    }
    const auto actualChecksum = XXH64(header.data(), kGlobalHeaderSize - 8, 0);
    if (actualChecksum != *storedChecksum) {
        return makeError<ArchiveMetadata>(ErrorCode::kChecksumError,
                                          "FQC v2 global header checksum mismatch");
    }
    metadata_ = {.version = *version,
                 .profile = static_cast<DatasetProfile>(*profile),
                 .paired = (*flags & kFlagPaired) != 0U};
    stats_.encodedBytes = header.size();
    opened_ = true;
    return metadata_;
}

auto ArchiveReader::readFrame() -> Result<std::optional<std::vector<ReadRecord>>> {
    if (!opened_) {
        auto result = open();
        if (!result) {
            return makeError<std::optional<std::vector<ReadRecord>>>(result.error());
        }
    }
    if (finished_) {
        return std::optional<std::vector<ReadRecord>>{};
    }

    std::array<std::uint8_t, 4> magicBytes{};
    if (auto result = readExact(input_, magicBytes); !result) {
        return makeError<std::optional<std::vector<ReadRecord>>>(result.error());
    }
    Cursor magicCursor(magicBytes);
    auto magic = magicCursor.readU32();
    if (!magic) {
        return makeError<std::optional<std::vector<ReadRecord>>>(magic.error());
    }
    if (*magic == kFooterMagic) {
        return readFooter(magicBytes);
    }
    if (*magic != kFrameMagic) {
        return makeError<std::optional<std::vector<ReadRecord>>>(ErrorCode::kFormatError,
                                                                 "invalid FQC v2 frame marker");
    }

    std::array<std::uint8_t, kFrameHeaderSize> header{};
    std::copy(magicBytes.begin(), magicBytes.end(), header.begin());
    if (auto result = readExact(input_, std::span(header).subspan(4)); !result) {
        return makeError<std::optional<std::vector<ReadRecord>>>(result.error());
    }
    Cursor cursor(header);
    auto marker = cursor.readU32();
    auto headerSize = cursor.readU32();
    auto frameId = cursor.readU32();
    auto recordCount = cursor.readU32();
    auto rawIdsSize = cursor.readU64();
    auto idsSize = cursor.readU64();
    auto rawSequencesSize = cursor.readU64();
    auto sequencesSize = cursor.readU64();
    auto rawQualitiesSize = cursor.readU64();
    auto qualitiesSize = cursor.readU64();
    auto storedChecksum = cursor.readU64();
    (void)marker;
    if (!headerSize || !frameId || !recordCount || !rawIdsSize || !idsSize || !rawSequencesSize ||
        !sequencesSize || !rawQualitiesSize || !qualitiesSize || !storedChecksum ||
        *headerSize != kFrameHeaderSize || *frameId != stats_.frameCount || *recordCount == 0) {
        return makeError<std::optional<std::vector<ReadRecord>>>(ErrorCode::kFormatError,
                                                                 "invalid FQC v2 frame header");
    }
    if (metadata_.paired && *recordCount % 2 != 0) {
        return makeError<std::optional<std::vector<ReadRecord>>>(
            ErrorCode::kFormatError, "paired FQC v2 frame contains an incomplete pair");
    }
    for (const auto size : {*rawIdsSize,
                            *idsSize,
                            *rawSequencesSize,
                            *sequencesSize,
                            *rawQualitiesSize,
                            *qualitiesSize}) {
        if (size > maxFrameBytes_ || size > std::numeric_limits<std::size_t>::max()) {
            return makeError<std::optional<std::vector<ReadRecord>>>(
                ErrorCode::kFormatError, "FQC v2 frame exceeds configured size limit");
        }
    }

    auto peakMemory = estimateDecompressionPeak(static_cast<std::size_t>(*rawIdsSize),
                                                static_cast<std::size_t>(*idsSize),
                                                static_cast<std::size_t>(*rawSequencesSize),
                                                static_cast<std::size_t>(*sequencesSize),
                                                static_cast<std::size_t>(*rawQualitiesSize),
                                                static_cast<std::size_t>(*qualitiesSize),
                                                *recordCount);
    if (!peakMemory) {
        return makeError<std::optional<std::vector<ReadRecord>>>(peakMemory.error());
    }
    if (*peakMemory > memoryLimitBytes_) {
        return makeError<std::optional<std::vector<ReadRecord>>>(
            ErrorCode::kFormatError, "FQC v2 frame exceeds configured memory limit");
    }

    auto ids = readCompressed(*idsSize, *rawIdsSize);
    auto sequences = readCompressed(*sequencesSize, *rawSequencesSize);
    auto qualities = readCompressed(*qualitiesSize, *rawQualitiesSize);
    if (!ids || !sequences || !qualities) {
        const auto& error =
            !ids ? ids.error() : (!sequences ? sequences.error() : qualities.error());
        return makeError<std::optional<std::vector<ReadRecord>>>(error);
    }
    const auto checksum = logicalChecksum(*ids, *sequences, *qualities);
    if (checksum != *storedChecksum) {
        return makeError<std::optional<std::vector<ReadRecord>>>(ErrorCode::kChecksumError,
                                                                 "FQC v2 frame checksum mismatch");
    }
    auto records = decodeRawStreams(*ids, *sequences, *qualities, *recordCount);
    if (!records) {
        return makeError<std::optional<std::vector<ReadRecord>>>(records.error());
    }

    ++stats_.frameCount;
    stats_.recordCount += records->size();
    for (const auto& record : *records) {
        stats_.totalBases += record.sequence.size();
    }
    stats_.encodedBytes += kFrameHeaderSize + *idsSize + *sequencesSize + *qualitiesSize;
    globalChecksum_ = advanceGlobalChecksum(globalChecksum_, checksum);
    return std::optional<std::vector<ReadRecord>>(std::move(*records));
}

auto ArchiveReader::readCompressed(std::uint64_t compressedSize,
                                   std::uint64_t rawSize) -> Result<Bytes> {
    Bytes encoded(static_cast<std::size_t>(compressedSize));
    if (auto result = readExact(input_, encoded); !result) {
        return makeError<Bytes>(result.error());
    }
    return decompress(encoded, static_cast<std::size_t>(rawSize));
}

auto ArchiveReader::readFooter(std::span<const std::uint8_t> magicBytes)
    -> Result<std::optional<std::vector<ReadRecord>>> {
    std::array<std::uint8_t, kFooterSize> footer{};
    std::copy(magicBytes.begin(), magicBytes.end(), footer.begin());
    if (auto result = readExact(input_, std::span(footer).subspan(4)); !result) {
        return makeError<std::optional<std::vector<ReadRecord>>>(result.error());
    }
    Cursor cursor(footer);
    auto magic = cursor.readU32();
    auto size = cursor.readU32();
    auto frameCount = cursor.readU64();
    auto recordCount = cursor.readU64();
    auto totalBases = cursor.readU64();
    auto checksum = cursor.readU64();
    (void)magic;
    if (!size || !frameCount || !recordCount || !totalBases || !checksum || *size != kFooterSize ||
        *frameCount != stats_.frameCount || *recordCount != stats_.recordCount ||
        *totalBases != stats_.totalBases) {
        return makeError<std::optional<std::vector<ReadRecord>>>(ErrorCode::kFormatError,
                                                                 "FQC v2 footer totals disagree");
    }
    if (*checksum != globalChecksum_) {
        return makeError<std::optional<std::vector<ReadRecord>>>(ErrorCode::kChecksumError,
                                                                 "FQC v2 global checksum mismatch");
    }
    if (input_.peek() != std::char_traits<char>::eof()) {
        return makeError<std::optional<std::vector<ReadRecord>>>(
            ErrorCode::kFormatError, "trailing bytes after FQC v2 footer");
    }
    if (input_.bad()) {
        return makeError<std::optional<std::vector<ReadRecord>>>(
            ErrorCode::kIOError, "failed while checking the end of the FQC v2 archive");
    }
    stats_.encodedBytes += footer.size();
    finished_ = true;
    return std::optional<std::vector<ReadRecord>>{};
}

}  // namespace fqc::format
