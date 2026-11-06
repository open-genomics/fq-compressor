# fq-compressor 测试

本目录存放 FQC v2 的单元测试与端到端测试。

## 测试框架

- **Google Test (GTest)**：单元测试。

## 测试文件命名

- 单元测试：`*_test.cpp`。

## 目录结构

- `commands/` — 归档引擎与 profile 检测测试
- `common/` — 结构化错误测试
- `e2e/` — 端到端 CLI 测试（shell 脚本）
- `fixtures/` — 冻结归档 fixture（`sequential-v2/` 本族、`foreign-indexed-v2/` 异族 magic 识别）
- `format/` — FQC v2 线格式、完整性、内存有界测试
- `fuzz/` — libFuzzer harness（FASTQ 解析器与归档 reader，对应 `clang-fuzz` preset 与 fuzz-nightly CI）
- `io/` — FASTQ 解析器与流测试
- `pipeline/` — MPMC / reorder / 压缩与解压流水线 / 并行解析

## 运行测试

```bash
# 用指定 preset 跑全部测试
./scripts/test.sh clang-debug

# 按过滤器跑
./scripts/test.sh clang-debug 'V2Archive*'

# 直接用 ctest
ctest --test-dir build/clang-debug --output-on-failure
```
