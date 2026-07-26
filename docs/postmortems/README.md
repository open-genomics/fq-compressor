# 复盘记录

本目录收录 fq-compressor 开发中遇到的非平凡问题复盘，聚焦根因而非结果。
CHANGELOG 只记"做了什么"，这里记"为什么发生、如何定位、如何避免再犯"。

## 约定

- 文件名：`YYYY-MM-DD-slug.md`，日期用问题**定位当日**。
- 七节结构：症状 / 复现 / 调查 / 根因 / 修复 / 验证 / 后续与教训。
- 一篇只写一个问题；复杂事故可拆多篇互相引用。
- 修复合入后在对应 CHANGELOG "修复"条目末尾加 `→ 详见 docs/postmortems/...`。
- 新建复盘时复制 `TEMPLATE.md`。

## 状态与严重度

- 状态：`open` / `closed` / `wontfix`。状态变更时更新下表。
- 严重度：`critical`（数据丢失/安全）/ `high`（核心功能损坏）/ `medium`（降级但可用）/ `low`（外观/ nuisance）。

## 索引

| 日期 | 问题 | 严重度 | 状态 |
|---|---|---|---|
| 2026-07-13 | v1 遗留架构债（ABC/SCM/reorder/无界内存） | high | closed |
| 2026-07-13 | `FastqParser::validateSequence` 忽略 `validBases` | high | closed |
| 2026-07-13 | 校验扫描致 Illumina 压缩回退到 45.84 MiB/s | medium | closed |
| 2026-07-13 | `GzipStreamBuf` move 丢失已解码缓冲 | high | closed |
| 2026-07-13 | 并发 preset 跑共享 fixture temp 冲突 | medium | closed |
| 2026-07-13 | LeakSanitizer/vptr 在受管 ptrace 环境不可用 | low | wontfix |
| 2026-07-13 | Conan 工具链漂移（zlib-ng/Clang21/CMake export） | medium | closed |
| 2026-07-24 | SPSC `pop()` 在 `close` 后丢失尾帧 | high | closed |
| 2026-07-26 | 阶段 D 多帧并行编码未提速（Amdahl） | low | closed |
