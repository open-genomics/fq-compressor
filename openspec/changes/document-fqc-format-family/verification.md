# Verification: document-fqc-format-family

## Metadata

- Verification status: `Implemented — evidence recorded, awaiting reviewer`
- Implementation HEAD: `4e7acc78963337eb27b6784a8afe55b8e309e8e1`
- Verifier: `(implementer; independent reviewer pending)`
- Verified at: `2026-08-19`
- Ready to archive: `no`

## Scope audit

- Expected files/modules: `README.md`、格式文档首页、`openspec/`
- Actual changed files: `README.md`（首屏对照表 + 安装 PATH 提醒）、
  `openspec/changes/document-fqc-format-family/tasks.md`、`verification.md`
- Unexpected changes: 无（`ARCHITECTURE.md` 未引用格式族共存说明，无需同步；
  未改任何 C++ 源文件）
- Existing user changes preserved: 是——apply 前工作区仅有未跟踪的
  `openspec/changes/document-fqc-format-family/`（本 change 的 spec 文件），
  已保留；无其他用户改动被覆盖

## Requirement traceability

| Requirement | Scenario | Test/command | Result | Evidence summary |
|---|---|---|---|---|
| Same-name format family coexistence is documented | User opens the README first screen | `grep` 首屏对照表 | pass | README 第 4–15 行：两仓库对照表（仓库/实现语言/格式族 ID/完整 magic/访问模型/对方链接） |
| Same-name format family coexistence is documented | User reads about cross-implementation decode | `grep` magic/解码说明 | pass | README 第 13–14 行：扩展名不能判定格式、reader 必须检查 magic、不能互相解码；第 10–11 行含双方完整 magic |
| Same-name binary PATH risk is documented | User installs both binaries | `grep` 安装说明 PATH 提醒 | pass | README 第 47–49 行（源码构建节）：同名 `fqc` 二进制 `PATH` 覆盖风险与 `which fqc` 检查 |
| Coexistence claims stay within implemented behavior | User searches for automatic dispatch | `grep` 无自动分派声明 | pass | `grep -nE '自动分派\|自动识别' README.md` 无匹配；`grep -nE 'C\+\+ ?版\|Rust ?版'` 无匹配（无“C++/Rust 版本”暗示同格式） |

## Commands

| Command | Exit status | Result summary |
|---|---:|---|
| `git status --short` | 0 | `?? openspec/changes/document-fqc-format-family/`（spec 目录）+ ` M README.md` |
| `git rev-parse HEAD` | 0 | `4e7acc78963337eb27b6784a8afe55b8e309e8e1` = base commit，HEAD 无差异 |
| `git diff --check` | 0 | 无空白错误 |
| `grep -n '46 51 43 56 32 0D 0A 1A' README.md` | 0 | 命中第 10 行 |
| `grep -n '89 46 51 43 0D 0A 1A 0A' README.md` | 0 | 命中第 11 行 |
| `grep -nE 'C\+\+ ?版\|Rust ?版' README.md` | 1（期望无匹配） | 无“C++/Rust 版本”暗示同格式 |
| `grep -nE '自动分派\|自动识别' README.md` | 1（期望无匹配） | 无自动分派声明 |
| `grep -n 'PATH' README.md` | 0 | 命中第 47–49 行 |
| `./scripts/lint.sh format-check`（PATH shim 提供 clang-format-18→19） | 1 | 仅环境问题：本地无 clang-format-18，用 19 时在未改动文件上报告返回类型换行策略差异（AGENTS.md 已知 18/21 差异）；本 change 未触碰这些文件 |
| `git diff --check` | 0 | 确认 diff 仅含本仓库 README.md |

## Not run

- `./scripts/build.sh clang-debug` / `./scripts/test.sh clang-debug`：本 change 为纯文档
  change，不触碰 C++ 源码/测试；仓库标准门禁中的格式检查已按任务要求执行。
- Archive（4.1–4.3）：需 reviewer 确认 `Ready to archive: yes` 后执行；本次未归档。

## Residual risks

- 与 Rust 仓库 README 的对照表可能漂移；两个独立 change 需在同一契约下各自完成
  （`fq-compressor-rust` 的 `document-fqc-format-family` 独立进行中）。
- 本地 `clang-format` 版本（19）与 CI（18）不一致；format-check 以 shim 方式运行，
  报告的 C++ 违规为环境版本差异，与本 change 无关。
- 完整 magic 以 hex + escaped bytes（`FQCV2\r\n\x1A`）展示；若未来格式族 ID 或
  magic 变更，需同步本表。

## Verdict

实现完成，验收场景全部通过；`Ready to archive` 等待独立 reviewer 确认后置 yes 再归档。
