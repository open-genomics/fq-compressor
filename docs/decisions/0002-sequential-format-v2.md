# ADR-0002: 格式族 `fqc-sequential/v2` 与 Rust 实现并存

- 状态：**已采纳**
- 日期：2026-08-21
- 关联：`openspec/project.md`（`FQC-DEC-001`）、`openspec/specs/archive-format/`
- 上游决策：`openspec/changes/archive/2026-08-19-freeze-sequential-v2-format`

## 背景

生态内有两个同名实现（本仓库 C++ `fqc` 与
[fq-compressor-rust](https://github.com/open-genomics/fq-compressor-rust) 的 Rust `fqc`），
共用 `fqc`/`.fqc` 名字。若两者格式不兼容却同名，用户无法凭扩展名判断用哪个解码器。

## 决策

- 两个实现均保留 `fqc`/`.fqc` 名，格式**由 archive magic 区分**，互不兼容、不能互相解码。
- 本仓库格式定名为 **`fqc-sequential/v2`**：magic `46 51 43 56 32 0D 0A 1A`
  （`FQCV2\r\n\x1A`），顺序自校验帧布局。
- Rust 实现格式定名为 **`fqc-indexed/v2`**：magic `89 46 51 43 0D 0A 1A 0A`
  （PNG 风格 `\x89FQC\r\n\x1A\n`）。
- reader 必须检查 archive magic 而非扩展名来判定格式。

## 后果

- 优点：命名简单（两个实现都叫 `fqc`），格式归属一眼可辨；`fqc-sequential/v2`
  冻结为规范（见 `openspec/specs/archive-format/`），fixture 冻结于
  `tests/fixtures/sequential-v2/`。
- 代价：用户若同时安装两个二进制会互相覆盖（`PATH` 靠前者生效，README 已提示）；
  `.fqc` 扩展名无法单独判定格式。

## 验证

- `tests/frozen_fixture_test`：冻结 fixture 的 round-trip 与字节精确性。
- `tests/format_family_test`：识别 indexed 族 magic 并明确拒绝（不产生误导性错误）。
