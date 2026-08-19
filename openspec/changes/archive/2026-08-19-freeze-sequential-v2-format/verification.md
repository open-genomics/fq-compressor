# Verification: freeze-sequential-v2-format

- Status: `Completed`
- Ready to archive: `yes`
- Verifier: `implementing agent (self-verified)`
- Date: 2026-08-13 (second pass: frozen archives generated and wired into tests)

## Environment

- HEAD: `a53d21cf15fb0e2a265da587929bc40df8c39959` (audit base + docs/spec commits;
  format code unchanged since audit base)
- Working tree: clean before this pass
- Build: Conan 2.31.2 (pip `--user --break-system-packages`), clang 18.1.3 + libc++ 19,
  preset `clang-debug`; dependency install mirrors `.github/workflows/ci.yml`

## Requirement -> Evidence Matrix

| Requirement | Scenario | Evidence | Result |
|---|---|---|---|
| Sequential archive identity | Reader opens sequential archive | `include/fqc/format/archive.h`: `kArchiveVersion = 2`; `src/format/archive.cpp`: magic `FQCV2\r\n\x1A`; README identifies as `fqc-sequential/v2` | passed |
| V2 structural layout is normative | Inspect frozen archive | `tests/fixtures/sequential-v2/MANIFEST.md`: exact 32-byte header layout, 72-byte frame header, 40-byte footer, all field offsets | passed |
| Unsupported structural fields fail closed | Unsupported codec | `src/format/archive.cpp` rejects unsupported version, header size, flags, profile, codec, reserved; `archive_test.cpp` covers rejection | passed |
| Checksums and truncation are enforced | Truncated frame | `archive_test.cpp` covers header checksum, frame checksum, truncation; XXH64 enforced on header and frames | passed |
| Frozen reader compatibility | Future reader processes fixtures | `frozen_se.fqc` + `frozen_pe.fqc` committed with SHA-256 in MANIFEST; `frozen_fixture_test.cpp` decodes both and compares records/metadata to committed FASTQ expectations; 15/15 ctest passed | passed |
| Writer byte stability is scoped | Compression library changes | MANIFEST: "Writer byte stability is scoped: zstd payload may change; frozen archives protect decoder compatibility only" | passed |

## Command Results (second pass)

| Command | Exit status | Summary |
|---|---|---|
| `conan install . --output-folder=build/clang-debug --build=missing -s build_type=Debug -s compiler=clang -s compiler.version=18 -s compiler.libcxx=libc++ -s compiler.cppstd=23` | 0 | Deps installed (mirrors CI invocation) |
| `./scripts/build.sh clang-debug` | 0 | 74/74 targets built |
| `./scripts/test.sh clang-debug` | 0 | 15/15 passed (13 unit + frozen_fixture_test + e2e) |
| `fqc compress/verify/decompress` on both fixtures | 0 | Round-trip `cmp` byte-identical (SE and interleaved PE) |
| `sha256sum` fixture files | 0 | Hashes recorded in MANIFEST.md |
| `git diff --check` | 0 | No whitespace errors |

First pass (2026-08-13, same date, pre-build-environment): `./scripts/lint.sh format-check`
passed; build/test deferred.

## Notes

- The delta spec requires "deterministic SE and PE v2 archives"; both are now committed.
  The wider 4-fixture matrix from `maintenance-design/07` §4.2 (empty, iupac-case) is
  beyond this change's approved scope and can be added by a follow-up change.
- `frozen_fixture_test` locates fixtures via the `FQC_FIXTURE_DIR` compile definition
  (registered in `tests/CMakeLists.txt`), so it runs under any CTest working directory.
- clang-format is not installed in this environment; `frozen_fixture_test.cpp` was written
  to repo style (4-space indent, 100-col, project-headers-first) and should be confirmed
  by the CI format job.
- Format constants: magic `46 51 43 56 32 0D 0A 1A`, version 2, header 32B,
  frame header 72B, footer 40B, frame magic `FRM2`, footer magic `END2`.
