# Frozen Sequential v2 Archive Fixtures

## Purpose

These fixtures freeze minimal `fqc-sequential/v2` archives so that future
decoder changes can be caught by committed, verifiable byte streams. They are
a decoder compatibility contract, not a canonical-writer assertion.

## Generator

- **Source commit**: `a53d21cf15fb0e2a265da587929bc40df8c39959`
  (format code identical to audit base `1361d4e8628a210d8fbfd415a761e67a625fd6be`;
  the commits in between touch docs, build fixes and org references only)
- **Software version**: `0.3.0-rc1` (`VERSION`)
- **Build**: `./scripts/build.sh clang-debug`, clang 18.1.3 + libc++ 19,
  Conan 2.31.2 (zstd 1.5.7, xxhash 0.8.3)
- **Commands**:
  - `fqc compress -i input_se.fastq -o frozen_se.fqc`
  - `fqc compress -i input_r1.fastq -2 input_r2.fastq -o frozen_pe.fqc`

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
| `input_se.fastq` | committed | `23e50ad22b964c4b8376e77eb59d39179b786c96da43fdd418db0338255b1846` |
| `frozen_se.fqc` | committed | `2b1cc50edfa47dd8bd7881a4ee7bb7f4980e693c14d90d90b640fdd62ccc4edf` |
| `input_r1.fastq` | committed | `c35654f914e3dacf19dcbc9ac526b58e0c38ef735a69c206e3444eb1bfd19d49` |
| `input_r2.fastq` | committed | `f21799fffae7162cf93c73a70ce9729c69a52c09f32987edfe84d16761c74c72` |
| `frozen_pe.fqc` | committed | `11845bd85fa923ebc2e9ae77a8909539a64c2877588f383e1894f6157c97c685` |

## Expected decoded output

| Archive | Profile | Paired | Frames | Records | Bases | Expected FASTQ |
|---|---|---|---|---|---|---|
| `frozen_se.fqc` | illumina | no | 1 | 3 | 150 | `input_se.fastq` |
| `frozen_pe.fqc` | illumina | yes | 1 | 6 | 300 | `input_r1.fastq`/`input_r2.fastq` interleaved (R1,R2 per pair) |

`tests/format/frozen_fixture_test.cpp` decodes both archives and compares
records and metadata against these expectations.

## Notes

- Writer byte stability is scoped: zstd compression payload may change between
  library versions; frozen archives protect decoder compatibility only.
- Fixture regeneration is an explicit maintenance action requiring format
  review: rebuild from a known commit, re-run the commands above, update this
  manifest's hashes, and confirm `frozen_fixture_test` passes.
