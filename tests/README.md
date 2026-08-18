# fq-compressor tests

This directory contains focused unit and end-to-end tests for FQC v2.

## Testing frameworks

- **Google Test (GTest)** for unit tests

## Test file naming convention

- Unit tests: `*_test.cpp`

## Directory structure

- `commands/` — Archive engine and profile-detection tests
- `common/` — Structured error tests
- `e2e/` — End-to-end CLI tests (shell scripts)
- `format/` — FQC v2 wire-format, integrity, and memory-bound tests
- `io/` — FASTQ parser and stream tests
- `pipeline/` — MPMC / reorder / 压缩与解压流水线 / 并行解析

## Running tests

```bash
# Run all tests with a preset
./scripts/test.sh clang-debug

# Run with filter
./scripts/test.sh clang-debug 'V2Archive*'

# Run via ctest directly
ctest --test-dir build/clang-debug --output-on-failure
```

