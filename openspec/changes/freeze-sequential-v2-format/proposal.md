# Change Proposal: freeze-sequential-v2-format

## Metadata

- Status: `Completed`
- Repository: `open-genomics/fq-compressor`
- Audit base: `1361d4e8628a210d8fbfd415a761e67a625fd6be`
- Capability: `archive-format`
- Task IDs: `FQC-CPP-FMT-001`, `ORG-GOV-001`, `ORG-CONTRACT-001`
- Decision IDs: `FQC-DEC-001`

## Why

C++ reader/writer 已有较明确顺序 v2 layout 和错误检查，但测试主要证明 current writer/current reader 自洽，没有提交由已知发布/RC 版本生成的 frozen archive。未来格式回归可能在 round-trip 测试中同时变化而不被发现。

## Changes

**Sequential v2 decoder contract**

- From: layout 主要存在于实现和零散文档，缺少跨版本 frozen fixture。
- To: `fqc-sequential/v2` 的 identity、header/frame/footer、checksum 和 rejection 行为成为仓库内规范，并由合法/损坏 fixtures 保护。
- Reason: 为未来兼容、family dispatch、sanitizer 和稳定发布建立基线。
- Impact: characterization/contract change，不修改现有 archive bytes。

## Scope

- 顺序 v2 normative spec；
- 由审计/已发布版本生成的 SE/PE 小型 archives 与 manifest；
- header/frame/footer 损坏、截断、版本/codec/flags rejection tests；
- 文档/CHANGELOG 和仓库内 OpenSpec context。

## Out of scope

- 不修改 magic、header/frame/footer layout 或 codec；
- 不实现 Rust indexed 格式识别/解码；
- 不重构 archive engine；
- 不实施 sanitizer/fuzz/release change；
- 不改变 CLI 名、`.fqc` 后缀或 `verify` 行为。

## Compatibility and rollback

fixture 定义的是 reader compatibility，不默认承诺 writer 输出 canonical full-file bytes。若 characterization test 暴露现有文档和实现差异，以实现/已发布 fixture 为证据修订 proposal；不得在本 change 中顺手改变 format。

## Approval

- `FQC-DEC-001`: Accepted; this family remains `fqc-sequential/v2` under `fqc/.fqc`
- Apply approval: `authorized by organization owner`
