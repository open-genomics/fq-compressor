# Agent Note: CI 门禁补全（TSan / Release / libFuzzer / 工具链对齐）

Status: implemented

## Problem

- TSan 预设存在（`CMakePresets.json`）但 CI 从不跑——git log 里恰恰是竞态 bug
  在反复修（`close() must wake producers blocked on a full queue` 两连修）。
- Release 配置从未在 CI 编译，且发布产物用 `-march=native`（`FQC_PORTABLE` 默认
  OFF）——Release-only 的 IPO/LTO 失败到打 tag 才暴露，发布二进制在旧 CPU 上
  可能非法指令。
- 零 fuzzing：parser 与 `ArchiveReader` 消费不可信输入，全靠手写对抗用例。
- 工具链漂移：`lint.sh` 优先 `clang-tidy-21`，CI 钉 `clang-tidy-18`（本地干净 ≠
  CI 干净）；`pip install conan` 未钉版本、无 `conan.lock`（transitive 依赖浮动）。

## Decision

- **TSan job**：`ci.yml` 新增 `thread-sanitizer`，复用共享 conan-build action；
  先 `sudo sysctl -w vm.mmap_rnd_bits=28`（Ubuntu 24.04 runner 内核 ASLR 熵过高
  会让 TSan 影子内存映射失败），`TSAN_OPTIONS=halt_on_error=1`。
- **Release 进 CI + 可移植产物**：新增 `clang-release-portable` 预设
  （`FQC_PORTABLE=ON`，禁用 `-march=native`）；`ci.yml` 新增 `release-build` job
  每提交编译该预设并跑测试；`release.yml` 的发布构建切到该预设。
- **libFuzzer**：新增 `clang-fuzz` 预设与两个 harness
  （`tests/fuzz/fastq_parser_fuzz.cpp`、`archive_reader_fuzz.cpp`），
  `FQC_BUILD_FUZZERS=ON` 时注册为 CMake 目标（不注册 ctest，由 CI 直接跑）。
  `ci.yml` 新增 `fuzz` job 每目标短跑 90s（冻结 fixture 做种子）；新增
  `fuzz-nightly.yml` 每夜 600s 长跑并上传 corpus。
- **工具链对齐**：`lint.sh` 的 `detect_clang_tidy` 改为优先 `clang-tidy-18`
  （与 CI 一致）；`conan install` 全部加 `--lockfile=conan.lock`（已生成提交）；
  conan-build action 钉 `conan==2.31.2`。

## Alternatives considered

- **本地降 clang-18 / CI 升 clang-21 统一工具链** — 与既有决策
  `2026-07-24-ci-local-clang-version-divergence` 冲突（刻意并存）；tidy 版本统一
  是低成本的局部收敛，不动编译器。
- **fuzz 用 ASan+libc++** — 不可行：Debian/Ubuntu 的 `libclang_rt.fuzzer` 预编译
  库按 libstdc++ 构建，与 libc++ 存在 ABI 符号不兼容（`-lstdc++` 也救不了）。
  fuzz 预设整体走 libstdc++ 工具链（`inherits: base` 而非 `clang-base`，conan 侧
  `compiler.libcxx=libstdc++11`），并对 libstdc++ 13 的 `<expected>` 守卫
  （`__cpp_concepts >= 202002L` 在 clang-18 下未升）加 `-D__cpp_concepts=202002L`。
- **fuzz 在 ASan job 里顺带跑** — 职责混杂、时长不受控；独立 job 与夜间 workflow
  分工更清晰。

## Consequences

- **收益**：竞态、Release-only 编译错、解析/解码崩溃三类缺陷全部有自动化门禁；
  本地与 CI 的 tidy/conan 行为对齐。
- **代价**：CI 新增 3 个 job（时长增加）；fuzz 构建维护 libstdc++ 与 libc++ 两套
  工具链路径（已有文档说明）。
- **与 2026-08 `add-ci-sanitizer-gate` 变更的关系**：该项目当时明确"轻量门禁"
  不引入 fuzz；本次在格式冻结、项目进入维护期后重议，fuzz 从"未采用"变为
  "CI 短跑 + 夜间长跑"。

## Verification

- 本地：TSan 17/17、`clang-release-portable` 17/17、fuzz 双 harness 短跑无崩溃
  （parser ~35k exec/s、reader ~530k exec/s）。
- CI 配置为 YAML 静态检查通过（无语法错误）。
