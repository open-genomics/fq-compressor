# fq-compressor (C++) - Project Context

## Identity

- **Canonical repository**: `open-genomics/fq-compressor`
- **Product name**: `fqc`
- **Archive extension**: `.fqc`
- **Format family**: `fqc-sequential/v2`
- **Binary**: `fqc`
- **Lifecycle**: active development
- **Language**: C++23 (CMake + Conan)

## Core contracts

| Capability | Path | Description |
|---|---|---|
| `archive-format` | `openspec/specs/archive-format/` | Sequential v2 binary layout, header/frame/footer, checksums |

## External boundaries

- **`fq-compressor-rust`**: shares product name `fqc` and extension `.fqc` but uses a
  different format family (`fqc-indexed/v2`). Each reader must reject the other's magic.
- **Decision `FQC-DEC-001`**: both implementations keep `fqc` / `.fqc`; format family
  distinguished by archive magic, not suffix.

## Validation commands

```bash
./scripts/lint.sh format-check
./scripts/build.sh clang-debug
./scripts/test.sh clang-debug
```

## Authority rules

- `src/` is the implementation source of truth.
- Models must not commit, push, create PRs, or publish without explicit authorization.
- High-risk changes (format, compatibility) use lightweight OpenSpec changes.
