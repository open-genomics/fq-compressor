// =============================================================================
// fq-compressor - Shared FASTQ Record Type
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fqc {

/// @brief Allocator for byte buffers that are always fully overwritten before use.
///
/// `std::vector::resize(n)` value-initializes the new elements, which for byte
/// buffers is a full memset. Read/decompress paths resize and then immediately
/// fill every byte from I/O, so the zeroing is pure waste: it costs roughly
/// 8-30 ms per GiB of input. The default-construction path is made a no-op
/// (char-like implicit-lifetime types are created by obtaining storage, see
/// [basic.life]); the argument-taking path still performs a real construction,
/// so `assign`, `insert` and `push_back` keep their copy semantics.
///
/// Only valid for trivial implicit-lifetime element types. Bytes between
/// `resize(n)` and the first write are indeterminate: never read them.
template <class T>
struct NoInitAllocator {
    using value_type = T;

    NoInitAllocator() = default;
    template <class U>
    NoInitAllocator(const NoInitAllocator<U>& other) noexcept {
        (void)other;
    }

    auto allocate(std::size_t count) -> T* {
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }

    // 只接管「无参默认构造」这一条路径（resize 的值初始化）。带参的
    // construct 故意不提供：allocator_traits 会回退到 construct_at，
    // assign/insert/push_back/resize(n, v) 因此保持真实的复制语义。
    template <class U>
    void construct(U* pointer) noexcept {
        (void)pointer;
    }

    template <class U>
    auto operator==(const NoInitAllocator<U>& other) const noexcept -> bool {
        (void)other;
        return true;
    }
};

/// @brief Byte buffer for archive payloads: resize() does not zero-initialize.
using ByteBuffer = std::vector<std::uint8_t, NoInitAllocator<std::uint8_t>>;

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
    // comment 已含前导分隔空格，无需再为分隔符补 1 字节（旧口径的 +1 会多计）。
    return record.id.size() + record.comment.size() + record.sequence.size() +
        record.quality.size() + kCanonicalFastqFramingBytes;
}

}  // namespace fqc
