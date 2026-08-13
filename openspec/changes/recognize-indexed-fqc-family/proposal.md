# Change Proposal: recognize-indexed-fqc-family

## Metadata

- Status: `Completed`
- Task IDs: `FQC-FAMILY-001` (C++)
- Prerequisites: `FQC-DOC-001` / sequential freeze

## Why

C++ reader treated the Rust indexed magic as a generic “not an FQC v2 archive”.
Users need a clear other-known-family reject pointing at
`open-genomics/fq-compressor-rust`.

## Changes

Classify the first 8 bytes before version/header parsing; reject
`fqc-indexed/v2` with a locked unsupported-family message; keep unknown and
truncated magics distinct. Cover verify/decompress (no info subcommand).

## Out of scope

Decoding indexed archives, CLI identity fields, product rename.
