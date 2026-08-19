# Change Proposal: correct-verify-contract

## Metadata

- Status: `Archived`
- Task IDs: `FQC-CPP-DOC-001`
- Prerequisites: none (docs only)

## Why

README claimed `verify` could fully validate “without decompression”, but the
implementation fully decodes every frame and only skips writing FASTQ.

## Changes

Correct README/ARCHITECTURE wording: full decode + validation, no FASTQ output;
same-order CPU/memory cost as decompress; XXH64 is integrity, not cryptographic
authentication. No code or `--quick` changes.

## Out of scope

Changing verify implementation, archive bytes, or adding quick verify.
