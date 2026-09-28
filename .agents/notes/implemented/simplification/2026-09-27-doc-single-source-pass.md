# Agent Note: 文档单源化密度收敛

Status: implemented

## Problem

同一主题在多份文档里各有副本：流水线图在 AGENTS.md / README / ARCHITECTURE /
roadmap 四处；clone+build 命令在 README 两处、building.md、CONTRIBUTING 三处；
"合成数据不代表真实压缩比"在 README 性能节与已知限制节写两遍；阶段进度表在
roadmap 与 development-journey 逐字重复；CONTRIBUTING 整节复述 AGENTS.md 的
注释语言与代码风格约定。副本漂移已经发生：ALGORITHM.md 引用的吞吐数字
（53/182 MiB/s）与 README（148.84 MiB/s）不一致；building.md 指向 AGENTS.md
的"exit code 约定"在 AGENTS.md 中并不存在（实际在 `error.h` 的 `toExitCode`）。

## Decision

每个主题指定唯一权威文档，其余位置降为指针或删除：

| 主题 | 权威源 | 降级处 |
|---|---|---|
| 流水线形态 | `ARCHITECTURE.md` | AGENTS.md 压为 3 行；roadmap 基线图删；README 留图+精简 bullet |
| 算法原理 | `ALGORITHM.md` | README 保留 teaser；删过时吞吐数字改指 benchmarks；docs/pages 站点同口径（过时 53/182 MiB/s 同批清除） |
| 构建/工具链 | `docs/building.md` | CONTRIBUTING 快速开始改指针；exit code 死链改指 `error.h` |
| 实测数字 | `docs/benchmarks.md`（台账）+ `docs/real-corpus.md`（真实语料） | ARCHITECTURE 内联数字收敛为一行+指针 |
| 阶段史 | `docs/roadmap.md` | journey 删重复进度表，只留叙事 |
| 协作约定 | `AGENTS.md` | CONTRIBUTING 注释语言/代码风格两节改指针 |

方法论依据 `.agents/skills/writing-for-agents/`：AGENTS.md 是常驻上下文
（context load，逐字付费），只放指针级信息；副本只允许存在"就地警告"类
（如同名二进制 PATH 覆盖提醒），不进单源规则。

顺带做机械归组：CHANGELOG `[0.1.0]` 下散落的 9 个同类型小节合并为
新增/变更/修复/删除/文档/测试/构建 七组，内容逐字保留。

## Alternatives considered

- **合并 development-journey 进 roadmap** — journey 的叙事弧与"收获"是
  roadmap 的条目式日志承载不了的；只删了与 roadmap 逐字重复的进度表。
- **每处重复都保留以便读者不用跳转** — 副本漂移是已观察到的实际成本
  （见 Problem），不是假设；跳转一次的成本低于读到过时数字。

## Consequences

- **收益**：主题归属有唯一入口，改一处即可；README「文档」表补齐了
  postmortems、benchmarks、survey、journey、.agents/notes 五个原先未索引的
  入口；AGENTS.md 体积收缩（常驻上下文更便宜）。
- **代价与已知上限**：指针化增加一次跳转。行号引用漂移风险已消除——survey 与
  roadmap 的 `文件:行号` 全部改为 `文件#节标题`（引代码改指符号名），约定记入
  AGENTS.md「注释与文档语言」；postmortems/reviews 是时点快照，其行号引用属
  历史记录，刻意保留。

## Verification

`rg '\w+\.(md|cpp|h):\d+' docs/` 在存活文档中已无命中（残余命中均在
postmortems/reviews 时点快照内，含粘贴的测试输出，刻意保留）；README 各锚点
（#安装与获取 #快速开始 #核心算法 #高性能架构 #性能 #已知限制）保持原名；
`./scripts/notes.sh verify` 通过。
