# Tasks: document-fqc-format-family

## 1. Baseline and tests

- [x] 1.1 记录 `git status --short`、HEAD 与 base commit 差异
      （`?? openspec/changes/document-fqc-format-family/`；HEAD = base =
      `4e7acc78963337eb27b6784a8afe55b8e309e8e1`，无差异）
- [x] 1.2 记录当前 README 首屏格式族说明；运行 `git diff --check`
      （原首屏：单行 blockquote，`fqc-sequential/v2` + magic + 一句与 Rust 区分；
      `git diff --check` 退出 0）
- [x] 1.3 添加文档一致性检查（grep/测试）：首屏共存对照表、完整 magic、
      无“C++/Rust 版本”暗示同格式、安装 `PATH` 风险提醒
      （具体 grep 命令见 3.1，全部通过）

## 2. Implementation

- [x] 2.1 在 README 首屏添加两个 `fqc`/`.fqc` 同名共存对照表（仓库、实现语言、
      格式族 ID、完整 magic、访问模型、对方链接）——README 第 4–15 行
- [x] 2.2 明确扩展名不能判定格式、reader 必须检查 magic、两个实现不能互相解码
      ——README 第 13–14 行
- [x] 2.3 安装说明提醒同名二进制 `PATH` 覆盖风险——README 第 47–49 行
- [x] 2.4 检查不再使用“C++/Rust 版本”暗示同格式的措辞；不宣称跨实现自动分派
      ——`grep -nE 'C\+\+ ?版|Rust ?版'` 与 `grep -nE '自动分派|自动识别'` 均无匹配

## 3. Verification

- [x] 3.1 运行文档一致性检查（2026-08-19，全部通过）：
  - `grep -n '46 51 43 56 32 0D 0A 1A' README.md`（完整 magic）→ 命中第 10 行
  - `grep -n '89 46 51 43 0D 0A 1A 0A' README.md`（对方 magic）→ 命中第 11 行
  - `grep -nE 'C\+\+ ?版|Rust ?版' README.md`（无“C++/Rust 版本”暗示同格式）→ 无匹配
  - `grep -nE '自动分派|自动识别' README.md`（无自动分派声明）→ 无匹配
  - `grep -n 'PATH' README.md`（安装 PATH 风险提醒）→ 命中第 47–49 行
  - 首屏（前 16 行）含 `fqc`/`.fqc` 共存对照表 → 命中第 4–15 行
- [x] 3.2 运行仓库标准门禁：`./scripts/lint.sh format-check`
      ——本地仅有 clang-format-19（无 18）；以 PATH shim 运行脚本后，在未改动的
      C++ 文件上报告 clang-18/19 返回类型换行策略差异（AGENTS.md 已知环境差异），
      非本 change 引入；本 change 仅改 README.md
- [x] 3.3 逐条核对 delta spec scenarios 并填写 `verification.md`
- [x] 3.4 运行 `git diff --check`，确认 diff 仅含本仓库文档——退出 0，仅 README.md

## 4. Archive readiness

- [ ] 4.1 Reviewer 确认 `verification.md` 的 `Ready to archive: yes`
- [ ] 4.2 将 delta 同步到主规格
- [ ] 4.3 按日期归档 change
