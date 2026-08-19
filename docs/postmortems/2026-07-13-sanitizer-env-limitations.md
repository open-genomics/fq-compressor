# LeakSanitizer/vptr 在受管 ptrace 环境不可用

- 日期：2026-07-13
- 严重度：low
- 状态：wontfix
- 引入点：v2 phase 7（pre-75b7400）
- 相关：`cmake/fqc_sanitizers.cmake`、`scripts/build.sh`

## 症状

- `LeakSanitizer` 在受管环境的 ptrace 包装层下，于测试退出前中止，导致 `./scripts/test.sh clang-asan` 在启用泄漏检测时无法完成。
- 该环境中 Clang 21 的 ASan/UBSan 运行时缺少 `vptr` C++ 符号，vptr 子检查无法链接。

## 复现

在受管（ptrace 受限）环境中，使用默认 `ASAN_OPTIONS` 运行 `./scripts/test.sh clang-asan`。

## 调查

- 确认 LeakSanitizer 在进程退出时需要 ptrace 来检查进程，而受管环境的包装层不允许该操作。
- 确认缺失的 vptr 符号属于运行时打包问题，并非代码缺陷。

## 根因

环境限制，而非应用 bug。LeakSanitizer 需要 ptrace；vptr 检查需要已安装的 Clang 21 包中缺失的运行时符号。

## 修复

- 以 `ASAN_OPTIONS=detect_leaks=0` 运行 ASan+UBSan，使地址检查与未定义行为检查仍可执行；泄漏检测记录为环境不可用。
- 在 sanitizer preset 中禁用不可用的 `vptr` 子检查，同时保留 ASan 与 UBSan 启用。

## 验证

`ASAN_OPTIONS=detect_leaks=0 ./scripts/test.sh clang-asan` 通过 8/8；`./scripts/test.sh clang-tsan` 通过 8/8。

## 后续与教训

泄漏检测与 vptr 检查仍作为发布环境验证项：在打 release tag 之前，必须在不受限的机器上运行。已记录于 `README.md`（基线文件原载于 `performance/baselines/2026-07-13-v0.2.0.md`，已随项目瘦身删除）。
