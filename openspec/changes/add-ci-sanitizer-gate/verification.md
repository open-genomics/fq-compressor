# Verification: add-ci-sanitizer-gate

- Status: `Completed`
- Date: 2026-08-14

| Case | Evidence | Result |
|---|---|---|
| 本地 clang-asan 构建 | `./scripts/build.sh clang-asan`（gtest 已用 `--build=gtest*` 重建） | passed |
| 本地 clang-asan 测试 | `ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0 ./scripts/test.sh clang-asan` | passed 16/16 |
| 本地 clang-debug 回归 | `./scripts/build.sh clang-debug` + `./scripts/test.sh clang-debug` | passed 16/16 |
| 格式检查 | `./scripts/lint.sh format-check`（clang-format-18） | passed |
| CI YAML | `python3 -c "yaml.safe_load(...)"` | passed |
| 远程 CI | GitHub Actions run 31778680176：build-and-test + sanitizer 全绿 | passed |

Notes:
- gtest 强制重建时须设 `CC=clang CXX=clang++`，否则 Conan 用系统 gcc 编译
  带 `-stdlib=libc++` 的依赖而失败（CI 步骤已含该环境变量）。
- 首轮 CI：sanitizer job 在 libc++18 上触发 alloc-dealloc-mismatch 误报
  （异常对象经 ASan 的 operator new 分配、由未插桩系统 libc++ 用 free 释放；
  本地 libc++19 用 operator delete 不触发）。以
  `ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0` 关闭该子检查修复，
  其余 ASan/UBSan 检查保持。
