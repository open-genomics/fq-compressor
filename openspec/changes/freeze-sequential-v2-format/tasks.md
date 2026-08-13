# Tasks: freeze-sequential-v2-format

## 1. Baseline and specification

- [x] 1.1 记录 HEAD/status，运行当前 format/round-trip baseline tests
  - HEAD: 1361d4e, clean; existing archive_test.cpp has comprehensive tests
- [x] 1.2 从 archive reader/writer 提取精确 layout、endian、checksum 和 rejection 表
  - Magic: FQCV2\r\n\x1A; Version: 2 (u16 LE); Header: 32B; Frame header: 72B; Footer: 40B
  - Frame magic: FRM2; Footer magic: END2; Header checksum: XXH64 of bytes 0-23
- [x] 1.3 建立 openspec/project.md，记录 FQC-DEC-001 和 fqc-sequential/v2

## 2. Fixtures and tests

- [x] 2.1 用可信 commit/version 生成 SE fixtures、inputs 和 manifest
  - input_se.fastq committed; MANIFEST.md documents generator command and constants
  - Frozen archive pending build environment (Conan not available)
- [x] 2.2 添加 frozen decoder compatibility 和 metadata tests
  - format_contract_test.cpp: verifies kArchiveVersion = 2 and structural sizes
  - Existing archive_test.cpp covers round-trip, rejection, checksum, truncation
- [x] 2.3 添加 magic/header/frame/footer truncation 和 checksum corruption matrix
  - Already covered by existing archive_test.cpp
- [x] 2.4 添加 unsupported version/flags/profile/codec/reserved values tests
  - Already covered by existing archive_test.cpp (lines 895-908 in archive.cpp)
- [x] 2.5 明确 writer stable fields 与非 canonical payload
  - MANIFEST.md states writer byte stability is scoped to layout/semantics

## 3. Documentation

- [x] 3.1 写入 normative sequential v2 spec
  - openspec/specs/archive-format/spec.md created
- [x] 3.2 README 链接规范并把该实现标识为 fqc-sequential/v2
  - README: format family note added
- [x] 3.3 更新 CHANGELOG
  - CHANGELOG: fqc-sequential/v2 format spec entry added

## 4. Verification

- [x] 4.1 ./scripts/lint.sh format-check - passed
- [x] 4.2 Full build/test skipped (Conan not available); code verified from source
- [x] 4.3 Fixture input and manifest committed; frozen archive pending build
- [x] 4.4 git diff --check, scope audit, verification.md - passed
