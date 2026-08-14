# Verification: add-ci-sanitizer-gate

- Status: `In Progress`（远程 CI 确认后置为 Completed）
- Date: 2026-08-14

| Case | Evidence | Result |
|---|---|---|
| 本地 clang-asan 构建 | `./scripts/build.sh clang-asan`（gtest 已用 `--build=gtest*` 重建） | passed |
| 本地 clang-asan 测试 | `ASAN_OPTIONS=detect_leaks=0 ./scripts/test.sh clang-asan` | passed 16/16 |
| 本地 clang-debug 回归 | `./scripts/build.sh clang-debug` + `./scripts/test.sh clang-debug` | passed 16/16 |
| 格式检查 | `./scripts/lint.sh format-check` | CI 执行（本地未装 clang-format-18） |
| CI YAML | `python3 -c "yaml.safe_load(...)"` | passed |
| 远程 CI | push 后 GitHub Actions build-and-test + sanitizer 全绿 | pending |

Notes:
- gtest 强制重建时须设 `CC=clang CXX=clang++`，否则 Conan 用系统 gcc 编译
  带 `-stdlib=libc++` 的依赖而失败（CI 步骤已含该环境变量）。
