# Foreign indexed fixture (for family rejection)

Copied from `open-genomics/fq-compressor-rust` `tests/fixtures/indexed-v2/frozen.fqc`
for `FQC-FAMILY-001` / `recognize-indexed-fqc-family`.

| Field | Value |
|---|---|
| Format family | `fqc-indexed/v2` |
| Magic | `89 46 51 43 0D 0A 1A 0A` |
| SHA-256 | `32081764a533a20704bb614624eb4d0a4651bcac8ecc4e4e7cc0e54ac37e7a03` |
| Expected C++ reader behavior | reject with unsupported format family |

This archive is **not** decoded by this repository.
