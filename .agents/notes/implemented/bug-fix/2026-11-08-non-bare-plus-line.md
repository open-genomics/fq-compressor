# Agent Note: reject non-bare `+` lines instead of silently rewriting them

Status: implemented

## Problem

FASTQ allows the third line to repeat the read identifier (`+id`, `+id desc`).
`FastqParser::readRecord` only required the line to start with `+`:

```cpp
FQC_TRY(readRequiredLine(plusLine));
if (plusLine.empty() || plusLine[0] != '+') { ... }
```

The archive stores only ID, comment, sequence and quality; on decompression
`writeFastqRecord()` always writes `"\n+\n"`. A LF-only, structurally valid
input such as:

```text
@r1
ACGT
+r1 desc
!!!!
```

therefore round-trips to a different file (`+r1 desc` -> `+`), with no warning.
That contradicts the README's byte-level lossless promise and can break
downstream tools that read the `+` line.

## Decision

Fail closed: a `+` line must be exactly one byte.

```cpp
if (plusLine.size() != 1) {
    return std::unexpected(formatError(
        "'+' line must be bare; per-read '+' content is not stored by the archive format"));
}
```

Line endings are already normalized (`trimTrailingCr`), so CRLF `+\r\n` is
handled as a bare `+`. README and ALGORITHM's lossless-boundary sections now
list non-bare `+` lines among the rejected structural variants.

## Alternatives considered

- **Store the `+` suffix in the archive.** The only way to keep full byte-level
  round-trip for these files, but it changes the on-disk format: the ID stream
  would need a per-record length+payload and `kArchiveVersion` would have to
  bump, making old archives unreadable (or requiring dual decoders). That is a
  much larger change than the audit's P2 and is not justified by current
  corpus needs.
- **Document the loss without rejecting.** Cheaper, but a compressor silently
  changing a valid input is exactly the failure mode the project treats as a
  correctness bug (see the parallel-parse silent-drop postmortem). Refusing is
  safer than producing an archive that cannot round-trip.
- **Normalize `+id` to bare `+` and count it as a known lossy case.** Still
  silently violates the lossless contract; rejected.

## Consequences

Benefits:

- No input is silently rewritten on the `+` line; unsupported inputs fail with
  a line-numbered `kFormatError` before any archive is produced.
- Old archives still decode; the on-disk format is unchanged.

Costs and limits:

- FASTQ files that repeat the identifier on the `+` line are no longer
  accepted. Users can strip the suffix (`sed '3~4s/^+.*/+/'`) before
  compression.
- Supporting them later requires a format version bump; this note records that
  choice as deliberately deferred, not forgotten.

## Verification

- New `FastqParserTest.RejectsNonBarePlusLine`: `+read1` returns
  `kFormatError` with "bare" in the message.
- `./scripts/test.sh clang-debug`: **19/19 test binaries passed**.
- Repro from the audit with the rebuilt binary:
  `printf '@r1\nACGT\n+r1 desc\n!!!!\n' | fqc compress ...` now exits 3 with
  `invalid FASTQ at line 3: '+' line must be bare; per-read '+' content is not
  stored by the archive format`.
- Bare `+` round-trip still passes (`compress` + `decompress` + `cmp`).
- `clang-format --dry-run --Werror` on the two modified files: clean.
  `./scripts/lint.sh format-check` still reports pre-existing violations in
  `tests/commands/archive_engine_test.cpp` because only clang-format 21 is
  installed locally while CI pins 18 (documented divergence).
- Not run: ASan/TSan presets for this specific change.
