# Design: recognize-indexed-fqc-family

`ArchiveReader::open` reads 8 magic bytes first, then classifies:

- indexed magic → `kUnsupportedCodec` with locked family message
- sequential magic → continue header parse
- other → `kFormatError` unknown magic
- short read → truncated magic

Fixture: copy of Rust `frozen.fqc` under `tests/fixtures/foreign-indexed-v2/`.
Verify/decompress share `ArchiveReader` via the decompress pipeline.
