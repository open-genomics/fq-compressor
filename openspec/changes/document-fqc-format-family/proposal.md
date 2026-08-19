# Change Proposal: document-fqc-format-family

## Metadata

- Status: `Proposed`
- Repository: `open-genomics/fq-compressor`
- Base commit: `4e7acc78963337eb27b6784a8afe55b8e309e8e1`
- Capability: `format-governance`
- Task IDs: `FQC-DOC-001`
- Decision IDs: `FQC-DEC-001`
- Proposed at: `2026-08-19`

## Why

两个实现共享产品名 `fqc` 与扩展名 `.fqc`，但属于不同格式族：本仓库为
`fqc-sequential/v2`，`fq-compressor-rust` 为 `fqc-indexed/v2`。README 首屏已有
一行简短的格式族说明，但用户无法一眼看到完整对照（仓库、实现语言、格式族 ID、
完整 magic、访问模型、对方链接），也没有安装后同名二进制 `PATH` 覆盖风险的提醒。
用户可能误以为两个 `fqc` 是同一格式的不同实现，从而按错误格式族使用。

## Evidence

| Fact | Repository evidence | Verification |
|---|---|---|
| 本仓库格式族为 `fqc-sequential/v2` | `README.md` 首屏格式族说明 | `head -8 README.md` |
| 另一个实现使用 `fqc-indexed/v2` | `README.md` 首屏格式族说明 | `grep -n 'fqc-indexed' README.md` |
| 扩展名不能判定格式，reader 拒绝对方 magic | `openspec/project.md` External boundaries | `grep -n 'reject' openspec/project.md` |
| 两个实现均保留 `fqc`/`.fqc` | `openspec/project.md`（`FQC-DEC-001`） | `grep -n 'FQC-DEC-001' openspec/project.md` |

## Changes

**Same-name format family coexistence contract (documentation)**

- From: README 只有一行简短格式族说明，缺少完整对照表和安装风险提醒。
- To: README 首屏提供「两个 `fqc`/`.fqc` 是同名、不同格式族产品」的对照表
  （仓库、实现语言、格式族 ID、完整 magic、访问模型、对方链接），明确扩展名
  不能判定格式、reader 必须检查 magic、两个实现不能互相解码，并提醒同名二进制
  `PATH` 覆盖风险。
- Reason: 让用户在进入任一仓库时立即知道两个 `fqc`/`.fqc` 是同名、不同格式族
  的产品，避免按错误格式族使用。
- Impact: 纯文档契约修正；不改变任何二进制行为、magic、格式或 CLI。

## Scope

- `README.md` 及格式文档首页；
- 仓库内 `format-governance` delta spec；
- 文档一致性搜索检查。

## Out of scope

- 不实现跨实现自动分派/识别（后续 `expose-sequential-fqc-identity` 等 change）；
- 不修改 archive bytes、magic、CLI 或格式；
- 不更改产品名或 `.fqc` 后缀；
- 不修改 Rust 仓库（该仓库独立 change）。

## Compatibility and rollback

不改格式、CLI、schema，仅文档。回滚可直接还原 README，不影响已生成 archive。

## Dependencies and blockers

- `FQC-DEC-001`：两个实现均保留 `fqc`/`.fqc`，格式族由 archive magic 区分 —— 已批准。

## Rollback

还原 README 与格式文档首页的改动即可；无已生成数据受影响。

## Approval

- Approved change scope: `pending`
- Approved breaking values: none
- Approved by: organization owner
