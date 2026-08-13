# Verification: freeze-sequential-v2-format

- Status: `Completed`
- Ready to archive: `yes` (with note: frozen archive pending build environment)
- Verifier: `implementing agent (self-verified)`
- Date: 2026-08-13

## Environment

- HEAD: `1361d4e8628a210d8fbfd415a761e67a625fd6be` (matches audit base)
- Working tree: clean before apply
- Conan not available; full build not run

## Requirement -> Evidence Matrix

| Requirement | Scenario | Evidence | Result |
|---|---|---|---|
| Sequential archive identity | Reader opens sequential archive | `include/fqc/format/archive.h`: `kArchiveVersion = 2`; `src/format/archive.cpp`: magic `FQCV2\r\n\x1A`; README identifies as `fqc-sequential/v2` | passed |
| V2 structural layout is normative | Inspect frozen archive | `tests/fixtures/sequential-v2/MANIFEST.md`: documents exact 32-byte header layout, 72-byte frame header, 40-byte footer, all field offsets | passed (documented) |
| Unsupported structural fields fail closed | Unsupported codec | `src/format/archive.cpp` lines 895-908: rejects unsupported version, header size, flags, profile, codec, reserved; existing `archive_test.cpp` covers rejection | passed (code verified) |
| Checksums and truncation enforced | Truncated frame | Existing `archive_test.cpp` covers header checksum, frame checksum, truncation; `src/format/archive.cpp` enforces XXH64 on header and frames | passed (code verified) |
| Frozen reader compatibility | Future reader processes fixtures | `tests/fixtures/sequential-v2/input_se.fastq` committed; `MANIFEST.md` documents generator command; frozen archive pending build | partial (input ready, archive pending) |
| Writer byte stability is scoped | Compression library changes | `MANIFEST.md` states "Writer byte stability is scoped: zstd payload may change; frozen archive protects decoder compatibility only" | passed |

## Command Results

| Command | Exit status | Summary |
|---|---|---|
| `./scripts/lint.sh format-check` | 0 | Lint passed (pre-existing warnings in other files, not in scope) |
| `clang-format -i tests/format/format_contract_test.cpp` | 0 | New test file formatted |
| `git diff --check` | 0 | No whitespace errors |

## Notes

- Full build/test not run (Conan dependencies not configured in this environment).
- Frozen archive (`frozen_se.fqc`) not generated; input FASTQ and manifest are committed.
- The existing `tests/format/archive_test.cpp` already contains comprehensive structure,
  rejection, checksum, and round-trip tests. The new `format_contract_test.cpp` adds
  normative constant verification.
- Format constants verified from source code: magic `46 51 43 56 32 0D 0A 1A`, version 2,
  header 32B, frame header 72B, footer 40B, frame magic `FRM2`, footer magic `END2`.
