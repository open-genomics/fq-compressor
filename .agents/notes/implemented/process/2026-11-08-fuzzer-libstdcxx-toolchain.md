# Agent Note: libFuzzer 只能配 libstdc++ 工具链

Status: implemented

## Problem

`FQC_BUILD_FUZZERS=ON` 时两个 harness 都能编译通过，但链接失败：

```
undefined symbol: std::__cxx11::basic_string<...>::_M_append
```

原因不是 harness 代码，而是工具链：本机 clang 自带的 libFuzzer 运行时
（`libclang_rt.fuzzer-x86_64.a`）是按 libstdc++ 编译的，而项目默认预设
（`clang-base`）全局使用 `-stdlib=libc++`。两套标准库的 ABI 名字不同
（`std::__1` vs `std::__cxx11`），libFuzzer 运行时引用的 libstdc++ 符号在
libc++ 链接线上无处解析。

CI 的 `fuzz` job 早就通过 `conan-libcxx: libstdc++11` 规避了这个问题，但本地
直接按常规流程配置 `clang-fuzz` 预设时会复现，且报错指向 `std::string`，
容易被误判成代码问题。

## Decision

- 本地复现步骤固定为：用 `compiler.libcxx=libstdc++11` 重新执行
  `conan install --output-folder=build/clang-fuzz ...`，再
  `./scripts/build.sh clang-fuzz`。该预设本来继承 `base`（不强制 libc++），
  只有 Conan 工具链决定 `-stdlib`。
- 在 `tests/CMakeLists.txt` 的 `FQC_BUILD_FUZZERS` 分支增加配置期守卫：
  检测到 `CMAKE_CXX_FLAGS` 含 `-stdlib=libc++` 时直接 `FATAL_ERROR`，并打印
  正确的 `conan install` / `build.sh` 命令。
- 没有把 fuzz 目标单独改成 `-stdlib=libstdc++`：`fqc_core` 仍以 libc++ 构建，
  跨库传递 `std::string`/`std::vector` 会出现真正的 ABI 不匹配，比链接失败更危险。

## Verification

- `conan install`（libstdc++11）后 `./scripts/build.sh clang-fuzz` 构建成功。
- 实跑（ASan + UBSan，种子取 `tests/fixtures/sequential-v2/`）：
  - 40 秒首轮：`fastq_parser_fuzz` 1,163,777 runs / 28k exec/s，
    `archive_reader_fuzz` 2,201,519 runs / 54k exec/s，均无崩溃；
  - 修正 `readLine` EOF 判定后重建，再各跑 25 秒：
    parser 665,389 runs / 26k exec/s，archive 1,223,090 runs / 47k exec/s，
    仍无崩溃。
- 两个 harness 的 crash 产物均为空。

## Alternatives considered

- **只给 fuzz 目标加 `-stdlib=libstdc++`**：目标 TU 与 `fqc_core`（libc++）之间的
  `std::string`/`std::vector` 会跨 ABI 传递，属于未定义行为，不能接受。
- **从源码用 libc++ 重编 compiler-rt 的 libFuzzer 运行时**：可行，但需要匹配
  clang 版本的 compiler-rt 源码与额外构建步骤，对本地复现和 CI 都是新负担。
- **只在 CI 跑 fuzz、本地不提供路径**：CI 本来就能跑，但本地无法验证 harness
  改动，用户要求的“实跑”仍然落空。

## Consequences

工具链错误在 CMake 配置期就以可操作的提示失败，不会再伪装成链接期的
`std::__cxx11` 未定义符号。CI 配置不变；本地需要按提示使用 libstdc++ 的
Conan profile 构建 fuzz 预设。
