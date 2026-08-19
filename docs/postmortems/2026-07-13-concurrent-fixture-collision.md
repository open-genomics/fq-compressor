# 并发 preset 共享 fixture 临时目录冲突

- 日期：2026-07-13
- 严重度：medium
- 状态：closed
- 引入点：v2 phase 7（pre-75b7400）
- 相关：`tests/io/compressed_stream_test.cpp`、`tests/CMakeLists.txt`

## 症状

并发运行 `./scripts/test.sh clang-debug` 与 `./scripts/test.sh clang-release` 时，其中一个 preset 的 `compressed_stream_test` 会删除另一个仍在使用的压缩 fixture，导致间歇性 "file not found" 失败。

## 复现

在两个 shell 中同时执行：

```bash
./scripts/test.sh clang-debug
./scripts/test.sh clang-release
```

## 调查

- 失败呈间歇性，且均指向同一条临时（temp）路径。
- 检查 fixture 初始化逻辑：两个 preset 共用 `/tmp` 下一个硬编码的共享临时目录。

## 根因

fixture 目录未按进程隔离，并发 preset 跑因此操作同一批文件。

## 修复

按进程 ID（PID）隔离 fixture 目录，使每次测试运行获得进程唯一的临时目录树。

## 验证

修改后并发运行 `clang-debug` 与 `clang-release` 测试，两者均 8/8 通过。

## 后续与教训

该模式现已在测试 CMake 配置中确立：任何写入临时目录的新 fixture 必须使用进程唯一 helper，而非共享路径。
