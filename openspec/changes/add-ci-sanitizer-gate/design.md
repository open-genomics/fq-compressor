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
