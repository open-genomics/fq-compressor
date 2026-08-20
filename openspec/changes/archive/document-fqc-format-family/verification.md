# Verification: document-fqc-format-family

## Metadata

- Verification status: `Implemented — ready to archive`
- Implementation HEAD: `b85d564`（收尾批次基座；最终 README 形态随收尾提交）
- Verifier: implementer（收尾批次）；reviewer 确认：项目 owner（2026-08-20）
- Verified at: `2026-08-20`
- Ready to archive: `yes`

## Scope audit

- Expected files/modules: `README.md`、格式文档首页、`openspec/`
- Actual changed files: `README.md`（首屏格式族 blockquote：双方完整 magic +
  扩展名/magic/不可互相解码说明 + PATH 覆盖提醒）、`docs/building.md`（新增，
  承载完整工具链与质量门禁说明）、`docs/fastq-compression-survey.md`（链接修正）、
  `openspec/changes/document-fqc-format-family/tasks.md`、`verification.md`
- Unexpected changes: 无（`ARCHITECTURE.md` 未引用格式族共存说明，无需同步；
  未改任何 C++ 源文件）
- Existing user changes preserved: 是——收尾批次保留工作树未提交的 `README.md`
  精简重构、`docs/fastq-compression-survey.md` 链接修正与新文件 `docs/building.md`，
  并在 README 补齐契约要点后一并归档。

## Requirement traceability

| Requirement | Scenario | Test/command | Result | Evidence summary |
|---|---|---|---|---|
| Same-name format family coexistence is documented | User opens the README first screen | `grep` 首屏格式族说明 | pass | README 第 5–8 行：首屏 blockquote 含双方格式族 ID、实现语言（Rust）、对方仓库链接、双方完整 magic |
| Same-name format family coexistence is documented | User reads about cross-implementation decode | `grep` magic/解码说明 | pass | README 第 8 行：扩展名不能判定格式、reader 必须检查 magic、不能互相解码；第 5、7 行含双方完整 magic |
| Same-name binary PATH risk is documented | User installs both binaries | `grep` 安装说明 PATH 提醒 | pass | README 第 33–34 行（快速开始节）：同名 `fqc` 二进制 `PATH` 覆盖风险与 `which fqc` 检查；`docs/building.md` 第 29–31 行为完整版 |
| Coexistence claims stay within implemented behavior | User searches for automatic dispatch | `grep` 无自动分派声明 | pass | `grep -nE '自动分派\|自动识别' README.md` 无匹配；`grep -nE 'C\+\+ ?版\|Rust ?版'` 无匹配（无“C++/Rust 版本”暗示同格式） |

## Commands

| Command | Exit status | Result summary |
|---|---:|---|
| `grep -n '46 51 43 56 32 0D 0A 1A' README.md` | 0 | 命中第 5 行 |
| `grep -n '89 46 51 43 0D 0A 1A 0A' README.md` | 0 | 命中第 7 行 |
| `grep -nE 'C\+\+ ?版\|Rust ?版' README.md` | 1（期望无匹配） | 无“C++/Rust 版本”暗示同格式 |
| `grep -nE '自动分派\|自动识别' README.md` | 1（期望无匹配） | 无自动分派声明 |
| `grep -n 'PATH\|which fqc' README.md` | 0 | 命中第 34 行 |
| `./scripts/lint.sh format-check`（PATH shim 提供 clang-format-18→19） | 1 | 仅环境问题：本地无 clang-format-18，用 19 时在未改动文件上报告返回类型换行策略差异（AGENTS.md 已知 18/21 差异）；本 change 未触碰这些文件 |
| `git diff --check` | 0 | 无空白错误 |

## Not run

- `./scripts/build.sh clang-debug` / `./scripts/test.sh clang-debug`：本 change 为纯文档
  change，不触碰 C++ 源码/测试；仓库标准门禁中的格式检查已按任务要求执行。
- Archive：已在 2026-08-20 收尾批次执行（spec 合并至 `openspec/specs/format-governance/`，
  变更目录移入 `openspec/changes/archive/`）。

## Residual risks

- 与 Rust 仓库 README 的格式族说明可能漂移；两个独立 change 需在同一契约下各自完成
  （`fq-compressor-rust` 的 `document-fqc-format-family` 独立进行中）。
- 本地 `clang-format` 版本（19）与 CI（18）不一致；format-check 以 shim 方式运行，
  报告的 C++ 违规为环境版本差异，与本 change 无关。
- 完整 magic 以 hex + escaped bytes（`FQCV2\r\n\x1A`）展示；若未来格式族 ID 或
  magic 变更，需同步本表。

## Verdict

实现完成，验收场景在最终 README 上全部通过；reviewer（项目 owner）已确认，
`Ready to archive: yes`，于 2026-08-20 收尾批次归档。
