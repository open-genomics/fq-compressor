# Agent Note: 引入 .agents/notes/ 决策笔记体系

Status: implemented

## Problem

项目的"为什么"记录分散在三处，且各有窄定位：`docs/decisions/` 只收难逆转、
影响外部读者的 ADR；`docs/postmortems/` 只收事故复盘；AGENTS.md 收协作规则。
日常非平凡改动（技术选型、被实测否掉的方案、跨文件契约变更）没有归属——
够不上 ADR 的分量，不是事故，也不该堆进常驻上下文的 AGENTS.md。这些意图
只活在 commit message 与记忆里，后续维护者（含 agent）会重走被否掉的老路，
或把已固化的妥协当成疏忽改掉。

## Decision

采用 write-notes-like-deepseek 的笔记体系：`.agents/notes/{proposed,implemented,
rejected,archived}/{feature,bug-fix,simplification,architecture,process,testing}/yyyy-mm-dd-topic.md`。
判定红线（机械改动不写、非平凡必写）、就地同步优先、决定翻转开新篇互链等纪律
以 skill 文档为准（`.agents/skills/write-notes-like-deepseek/SKILL.md`）。

与既有记录的分工（各司其职，不重复记录）：

- `.agents/notes/`：agent 工作层决策——非平凡改动的意图、被否方案与理由、
  取舍与重访触发条件。
- `docs/decisions/`（ADR）：维持原定位，只收难逆转、影响外部读者/下游工具的
  决策；notes 不复制其内容，需要时相对链接互引。
- `docs/postmortems/`：事故复盘照旧（七节结构）；复盘暴露出的"以后别再提"
  类方案级否决，可另立 `rejected/` 笔记防重提。
- `openspec/`：规范治理照旧；notes 不替代 openspec 变更流程。

校验入口 `scripts/notes.sh`（包装 skill 自带 tsx 脚本，经 npx 运行、零新增依赖）；
CI `notes` job 跑 `verify` 三线（结构、格式、归档封印）。

## Alternatives considered

- **扩展 docs/decisions/ ADR 收一切** — ADR README 明确收窄到"难逆转 + 外部
  读者"；扩收日常改动会稀释其定位，也让每条轻量笔记背上编号与评审流程的重量。
- **在 AGENTS.md 继续堆「已知权衡」** — AGENTS.md 是常驻上下文，每条权衡都让
  全体会话付 token；且纯列表没有 Problem/Alternatives 骨架，理由与触发条件会丢。
- **只靠 commit message** — commit 记"做了什么"，说不出"放弃了什么"；被否
  方案（如阶段 D 提速假设）只能靠翻 postmortem 偶遇，防不住重提。

## Consequences

- **收益**：非平凡改动的意图有固定落点；`rejected/` 防重复提案；`verify`
  脚本把格式纪律机械化，不靠自觉。
- **代价与已知上限**：写笔记有成本，红线清单（机械改动不写）必须守住，
  否则笔记通胀比没有更糟。notes 与 ADR 的双层分工边界需维护者把关，
  争议时以 ADR README 与本篇为准。

## Verification

`./scripts/notes.sh verify` 校验目录结构、头块格式与归档封印；CI `notes`
job 在 push/PR 上门禁。可用 `rg --hidden <关键词> .agents/notes/` 检索历史决策。
