# CI Quality Gates Specification

## ADDED Requirements

### Requirement: Sanitizer and concurrency gates are hard CI requirements
The CI SHALL run ASan+UBSan and TSan against the full test suite, and a failure in
either SHALL fail the build.

#### Scenario: Data race is introduced
- **GIVEN** a change introducing a data race in pipeline code
- **WHEN** the thread-sanitizer job runs the test suite
- **THEN** the job SHALL fail with `halt_on_error=1`

### Requirement: Release configuration is compiled and tested in CI
The CI SHALL build the Release configuration (IPO/LTO) with `FQC_PORTABLE=ON` on every
push and run the test suite against it, so Release-only breakage and non-portable
`-march=native` artifacts are caught before tagging.

#### Scenario: Release-only compile failure
- **GIVEN** a change that compiles in Debug but fails under Release/IPO flags
- **WHEN** the release-build job runs
- **THEN** the job SHALL fail

### Requirement: Fuzzing is a gated and scheduled quality activity
The repository SHALL maintain libFuzzer harnesses for the FASTQ parser and the archive
reader, run them briefly on every push and for a longer session on a nightly schedule,
and surface crash artifacts when a run fails.

#### Scenario: Fuzzer finds a crash
- **GIVEN** an input that triggers a sanitizer failure inside a harness
- **WHEN** the fuzz job runs
- **THEN** the job SHALL fail and upload the crash artifact

### Requirement: Static analysis and note-integrity gates are hard CI requirements
The CI SHALL run clang-tidy with warnings-as-errors on production sources, check
clang-format, and verify agent decision notes; any failure SHALL fail the build.

#### Scenario: clang-tidy warning
- **GIVEN** a change introducing a clang-tidy warning on a production source
- **WHEN** the clang-tidy job runs
- **THEN** the job SHALL fail
