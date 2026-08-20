# Design: document-fqc-format-family

## Context

`FQC-DEC-001` 已批准两个实现均保留 `fqc`/`.fqc`，格式族由 archive magic 区分。
`README.md` 首屏已有单行格式族说明（`fqc-sequential/v2` + magic + 一句与 Rust
实现区分），但缺少完整对照表、对方链接和安装 PATH 风险提醒。

## Goals

- 用户在进入任一仓库时立即知道两个 `fqc`/`.fqc` 是同名、不同格式族的产品；
- 完整 magic 以精确 hex/escaped bytes 展示；
- 明确扩展名不能判定格式、reader 必须检查 magic、两个实现不能互相解码；
- 安装说明提醒同名二进制 `PATH` 覆盖风险。

## Non-goals

- 不实现跨实现自动分派/识别；
- 不改产品名、`.fqc` 后缀、magic 或格式 bytes；
- 不修改对方仓库。

## Current flow

README 首屏：单行格式族说明（本仓库 `fqc-sequential/v2` + magic，一句与
`fq-compressor-rust` 的 `fqc-indexed/v2` 区分）。

## Target flow

README 首屏：同名共存对照表（仓库、实现语言、格式族 ID、完整 magic、访问模型、
对方链接）+ 安装 `PATH` 覆盖提醒；格式文档首页同步。

## Decisions

### Comparison table in README first screen

- Choice: 在 README 首屏（格式族说明处）放置对照表
- Reason: 用户进入仓库最先看到的位置；与现有格式族说明同处
- Alternatives rejected: 单独格式文档页（多一跳，首屏仍看不到）

## Allowed change surface

- `README.md`：首屏格式族说明扩展为对照表；
- 格式文档首页（若引用格式族说明）；
- `openspec/`：本 change 的 delta spec。

Files/modules outside this list require proposal revision and renewed approval.

## Contract and compatibility

纯文档 change，无 wire/CLI/schema 影响。

## Failure, resource and security behavior

无运行时代码改动，不涉及。

## Test and fixture design

| Requirement/scenario | Test level | Expected result |
|---|---|---|
| 共存说明位于首次格式介绍附近 | grep/文档检查 | 搜索 `.fqc` 时首屏可见共存对照表 |
| magic 完整展示 | grep/文档检查 | hex 或 escaped bytes 精确匹配 `46 51 43 56 32 0D 0A 1A` |
| 不再使用“C++/Rust 版本”暗示同格式 | grep/文档检查 | 无匹配 |
| 安装 PATH 风险提醒 | grep/文档检查 | README 安装说明含 `PATH` 提醒 |

## Risks and mitigations

| Risk | Likelihood/impact | Mitigation |
|---|---|---|
| 与 Rust 仓库说明漂移 | 中/低 | 两个独立 change 使用同一契约，各自提交 |

## Rollback details

还原 README 与格式文档首页改动即可；无已生成数据需要继续兼容。
