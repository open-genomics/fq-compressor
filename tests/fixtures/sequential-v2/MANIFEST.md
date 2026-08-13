# Frozen Sequential v2 Archive Fixture

## Purpose

This fixture freezes a minimal `fqc-sequential/v2` archive so that future
decoder changes can be caught by a committed, verifiable byte stream. It is a
decoder compatibility contract, not a canonical-writer assertion.

## Generator

- **Source commit**: `1361d4e8628a210d8fbfd415a761e67a625fd6be`
- **Binary**: `fqc compress -i input_se.fastq -o frozen_se.fqc`
- **Build**: `./scripts/build.sh clang-debug` (requires Conan)

## Format constants (from `include/fqc/format/archive.h` and `src/format/archive.cpp`)

| Constant | Value | Description |
|---|---|---|
| Archive magic | `46 51 43 56 32 0D 0A 1A` | `FQCV2\r\n\x1A` (8 bytes) |
| Archive version | 2 (u16 LE) | `kArchiveVersion` |
| Global header size | 32 bytes | `kGlobalHeaderSize` |
| Frame header size | 72 bytes | `kFrameHeaderSize` |
| Footer size | 40 bytes | `kFooterSize` |
| Frame magic | `0x324D5246` (u32 LE) | `FRM2` |
| Footer magic | `0x32444E45` (u32 LE) | `END2` |
| Flag paired | bit 0 | `kFlagPaired` |
| ID codec | 1 | `kIdCodecZstd` |
| Sequence codec | 1 | `kSequenceCodecPackedZstd` |
| Quality codec | 1 | `kQualityCodecZstd` |
| Header checksum | XXH64 of bytes 0-23 | Stored at offset 24 |

## Global header layout (32 bytes, little-endian)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | Magic (`FQCV2\r\n\x1A`) |
| 8 | 2 | Version (u16 LE) = 2 |
| 10 | 2 | Header size (u16 LE) = 32 |
| 12 | 4 | Flags (u32 LE) |
| 16 | 1 | Profile |
| 17 | 1 | ID codec |
| 18 | 1 | Sequence codec |
| 19 | 1 | Quality codec |
| 20 | 4 | Reserved (must be 0) |
| 24 | 8 | Header checksum (XXH64 of bytes 0-23) |

## Files

| File | Status | SHA-256 |
|------|--------|---------|
| `input_se.fastq` | committed | pending |
| `frozen_se.fqc` | pending build | pending |

## Notes

- Frozen archive generation requires a C++ build with Conan dependencies.
- The fixture input is committed; the archive will be generated when a build
  environment is available and committed with its SHA-256 hash.
- Writer byte stability is scoped: zstd compression payload may change between
  library versions; frozen archive protects decoder compatibility only.
