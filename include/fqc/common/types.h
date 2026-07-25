// =============================================================================
// fq-compressor - Shared FASTQ Record Type
// =============================================================================

#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace fqc {

/// @brief Owning representation of the four logical fields preserved by FQC v2.
struct ReadRecord {
    /// @brief Identifier without the leading '@'.
    std::string id;

    /// @brief Optional text following the first space on the header line.
    std::string comment;

    /// @brief IUPAC nucleotide sequence; case is significant and preserved.
    std::string sequence;

    /// @brief Phred+33 quality string; length must equal the sequence length.
    std::string quality;

    ReadRecord() = default;

    ReadRecord(std::string idValue, std::string sequenceValue, std::string qualityValue)
        : id(std::move(idValue)),
          sequence(std::move(sequenceValue)),
          quality(std::move(qualityValue)) {}

    ReadRecord(std::string idValue,
               std::string commentValue,
               std::string sequenceValue,
               std::string qualityValue)
        : id(std::move(idValue)),
          comment(std::move(commentValue)),
          sequence(std::move(sequenceValue)),
          quality(std::move(qualityValue)) {}

    [[nodiscard]] auto isValid() const noexcept -> bool {
        return !id.empty() && !sequence.empty() && sequence.size() == quality.size();
    }

    [[nodiscard]] auto length() const noexcept -> std::size_t {
        return sequence.size();
    }

    void clear() noexcept {
        id.clear();
        comment.clear();
        sequence.clear();
        quality.clear();
    }

    [[nodiscard]] auto operator==(const ReadRecord& other) const noexcept -> bool = default;
};

/// @brief Framing bytes of a canonical FASTQ record: '@' id [' ' comment] '\n' seq '\n' '+' '\n'
/// qual '\n'.
inline constexpr std::size_t kCanonicalFastqFramingBytes = 6;

/// @brief Size in bytes of the canonical FASTQ serialisation of @p record (for throughput
/// accounting).
[[nodiscard]] inline auto canonicalFastqBytes(const ReadRecord& record) noexcept -> std::size_t {
    return record.id.size() + record.comment.size() + record.sequence.size() +
        record.quality.size() + kCanonicalFastqFramingBytes + (record.comment.empty() ? 0 : 1);
}

}  // namespace fqc
