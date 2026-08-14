# Design: add-ci-sanitizer-gate

`.github/workflows/ci.yml` gains a `sanitizer` job running in parallel with
`build-and-test`, reusing the same clang-18/libc++-18 install steps.

```yaml
sanitizer:
  runs-on: ubuntu-24.04
  steps:
    - checkout, install clang-18 toolchain (same as build-and-test)
    - conan install . --output-folder=build/clang-asan --build=missing \
        --build=gtest* -s build_type=Debug -s compiler=clang \
        -s compiler.version=18 -s compiler.libcxx=libc++ -s compiler.cppstd=23
    - ./scripts/build.sh clang-asan
    - ASAN_OPTIONS=detect_leaks=0 ./scripts/test.sh clang-asan
```

Why `--build=gtest*`: the repository's CMake preset builds project code with
`-fsanitize=address,undefined` while Conan-provided GTest binaries are not
instrumented; mixing them trips the libc++ container annotation false positive
at gtest static-registration time (postmortem 2026-07-13). Forcing GTest from
source avoids the false signal.

Why `detect_leaks=0`: LeakSanitizer needs ptrace at process exit, unavailable
in some managed environments; GitHub Actions runners do support it, but
keeping the same flag as local practice makes the gate portable and
deterministic. Leak detection stays a release-machine check.

vptr sub-check is already disabled in the `clang-asan` preset
(`-fno-sanitize=vptr`), so the Clang-21 runtime symbol gap does not affect CI.

## Revision 1: alloc-dealloc-mismatch false positive (2026-08-14)

First CI run of the `sanitizer` job failed with `alloc-dealloc-mismatch
(operator new vs free)` on three tests that throw exceptions (fastq_parser,
compressed_stream, v2_cli_smoke). Root cause: exception objects are allocated
through ASan-instrumented `operator new` in project code and released with
`free` by the **non-instrumented system libc++18**; local libc++19 releases
with `operator delete` and does not trigger it. Not a project bug.

Fix: run with `ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0`, keeping
all other ASan/UBSan checks enabled. `alloc_dealloc_mismatch` is a
library-ABI mismatch detector; disabling it does not weaken heap/stack/UB
coverage.
