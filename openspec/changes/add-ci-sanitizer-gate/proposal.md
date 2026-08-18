# Change Proposal: add-ci-sanitizer-gate

## Metadata

- Status: `Completed`
- Task IDs: `FQC-CPP-SAN-001`
- Prerequisites: none (CI already covers clang-debug build + test + format)

## Why

CI only runs the plain clang-debug build/tests. Memory-safety and
undefined-behavior regressions (buffer overruns, out-of-bounds, signed
overflow, misaligned access) slip through until a release-machine check.
The repository already ships `clang-asan` and `clang-tsan` presets and a
postmortem (2026-07-13) documenting the local environment limits; a CI
ASan+UBSan gate makes the sanitizer signal a hard, per-commit requirement.

## Changes

Add a `sanitizer` job (or steps) to `.github/workflows/ci.yml` that:
- installs the same clang-18 toolchain;
- runs `conan install` into `build/clang-asan` with the same Debug settings;
- forces `--build=gtest*` from source so GTest is built with sanitizer
  flags and does not trigger the libc++ container-annotation false positive;
- runs `./scripts/build.sh clang-asan`;
- runs `ASAN_OPTIONS=detect_leaks=0 ./scripts/test.sh clang-asan` so the
  leak detector (ptrace-bound) is not a gate, while ASan/UBSan remain on;
- documents the leak-detector scope in CI comments.

TSan is intentionally not added as a CI gate: it needs more resources and is
covered by the local `clang-tsan` preset on the concurrency review path.

## Out of scope

- ThreadSanitizer in CI
- Fuzz harness in CI (project policy: lightweight gates only)
- Local environment fixes (CMakeUserPresets / sanitizer postmortem)
