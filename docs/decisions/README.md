# 决策记录（ADR）

本目录以轻量 ADR 形式固化**已定、难逆转、影响外部读者**的架构决策。
不记录常规实现细节——那些属于代码注释、postmortem 与 roadmap；
只收录"事后想改会伤筋动骨、且外部读者（协作者/下游工具）需要理解为什么"的决定。

## 约定

- 每篇一个决策，编号递增（`NNNN-<slug>.md`），状态写入首行。
- 引用既有治理：格式规范走 `openspec/specs/`，本目录承接跨实现/跨治理的**决策理由**。
- 新增决策流程见 [CONTRIBUTING.md](../../CONTRIBUTING.md) 与 `openspec/changes/`。

## 索引

| 编号 | 决策 | 状态 |
|---|---|---|
| [0001](0001-no-heavy-parallel-framework.md) | 不引重并行框架（TBB/线程池/无锁/异步 I/O） | 已采纳 |
| [0002](0002-sequential-format-v2.md) | 格式族 `fqc-sequential/v2` 与 Rust 实现并存 | 已采纳 |
