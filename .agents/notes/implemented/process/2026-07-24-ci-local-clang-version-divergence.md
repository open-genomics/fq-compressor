# Agent Note: CI 与本地 clang 版本刻意并存（18 vs 21）

Status: implemented

## Problem

CI（`.github/workflows/ci.yml`）用 clang-18（action 内联 `-s compiler.version=18`），
本地 conan profile 用 clang-21。同一套 C++23 代码经两个大版本编译，存在行为
分叉风险（诊断口径、libc++ 差异、sanitizer 行为）。2026-07 conan 工具链漂移
复盘（`docs/postmortems/2026-07-13-conan-toolchain-drift.md`）之后需要明确立场：
统一到哪一边，还是刻意维持差异。

## Decision

维持双版本并存，不统一。成立前提：项目未使用 clang-21 独有的 C++23 特性
（`<print>`、ranges 补全等），libc++ 18/21 在已用特性上行为一致。

重访触发条件（任一成立即重新统一）：

- 代码开始依赖 clang-21 独有特性；
- sanitizer 或诊断行为出现跨版本差异，导致本地绿 CI 红（或反之）。

## Alternatives considered

- **CI 升到 clang-21** — 对齐最直接，但需引入 apt.llvm.org 源，CI 镜像变重、
  安装变慢；而当时并未观察到实际分叉，收益只是消除假设中的风险，性价比不成立。
- **本地降到 clang-18** — 一致性的反面做法；放弃新版编译器的诊断能力与
  C++23 改进，对练手项目是倒退。

## Consequences

- **收益**：CI 镜像保持轻量；本地持续使用新工具链。
- **代价与已知上限**："本地绿不等于 CI 绿"的风险常驻（clang-tidy/asan/tsan
  版本差）；缓解手段是以 CI 为权威，本地结果仅供参考。一旦用上 21 独有特性
  或观察到跨版本行为差异，必须立即统一，不得拖延。

## Verification

CI workflow 与 `.github/actions/conan-build/` 内联 clang-18；本地 conan profile
为 clang-21；两侧 sanitizer 门禁（`scripts/test.sh clang-asan|clang-tsan`）
目前结果一致。
