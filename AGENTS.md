# AGENTS.md - fq-compressor

个人练习项目：基于 C++23 的高性能 FASTQ 压缩并发流水线。

## 构建与测试

```bash
./scripts/build.sh clang-debug      # 构建
./scripts/test.sh clang-debug       # 运行全部测试
./scripts/test.sh clang-asan        # sanitizer 构建 + 测试
./scripts/lint.sh format-check      # 检查格式
./scripts/lint.sh format            # 自动格式化
```

## 代码风格

- C++23，clang + libc++，4 空格缩进，100 列限制
- 命名：类型 PascalCase，函数/变量 camelCase，成员变量 `trailing_` 后缀，常量 `kPascal`
- 错误处理：`Result<T>`（即 `std::expected<T, Error>` 的别名），库代码里不用异常
- 日志：`fqc/log.h` 中的 `FQC_LOG_INFO(...)` 等宏（基于 fmt，输出到 stderr）
- 头文件顺序：项目头文件优先，其次标准库，最后第三方库

## 架构

```
压缩（普通文件）: parser×K 切块+边界对齐 →[MPMC]→ encoder×N(2-bit+zstd×3) →[MPMC]→ ChunkOrderer 保序写盘
压缩（gz/stdin/双端）: 单 reader 顺序解析，后两段同上
解压: reader(readRawFrame) →[MPMC]→ decoder×N →[MPMC]→ writer(reorder+滚动校验和)
```

数据流、内存模型与字节布局详见 `ARCHITECTURE.md`。核心模块：
- `include/fqc/pipeline/` — MPMC 队列、reorder、压缩/解压/并行解析流水线（核心学习点）
- `src/format/archive.cpp` — 二进制格式：varint、2-bit DNA 打包、zstd 帧、XXH64
- `src/io/` — FASTQ 解析、gzip 透明输入
- `src/commands/` — CLI 编排（compress/decompress/verify）

## 依赖（Conan）

cli11、fmt、zlib-ng、zstd、xxhash、gtest（仅测试）

## 注释与文档语言

- **公开头文件 API 注释用英文**（`include/fqc/**` 下的 Doxygen 风格 `///` 文档），
  保证跨语言协作者与工具链（Doxygen）可读。
- **实现内注释用中文**（`.cpp` 内的行内/块注释），解释"为什么"与取舍，不复述代码。
- 文档（README、docs/、CHANGELOG、openspec、tests/README）用中文。
- 文档互引用写 `文件#节标题`，代码引用写 `文件`+符号名；不写 `文件:行号`（行号随编辑漂移）。
- 新代码遵循上述策略；不要顺手翻写既有注释（避免无价值 diff）。

## Git

- 提交信息使用中文
- 格式：`<类型>: <简述>`，如 `refactor: 移除 Quill 依赖`、`feat: 添加 SPSC 并发流水线`

## 复盘

非平凡问题（竞态、难复现 bug、架构权衡变更）写复盘，不入 CHANGELOG 正文。
- 目录：`docs/postmortems/`，索引与约定见 `docs/postmortems/README.md`，模板见 `TEMPLATE.md`
- 文件名：`YYYY-MM-DD-slug.md`，七节：症状 / 复现 / 调查 / 根因 / 修复 / 验证 / 后续与教训
- CHANGELOG 对应"修复"条目末尾加 `→ 详见 docs/postmortems/...`

## 决策笔记（.agents/notes/）

非平凡改动（行为/架构/跨文件契约/工具链/测试策略/落盘格式）必须带一篇笔记；
机械改动（格式化、改名、依赖补丁、单模块显式修复）不写。

- 结构：`{proposed,implemented,rejected,archived}/{feature,bug-fix,simplification,architecture,process,testing}/YYYY-MM-DD-topic.md`
- 判定红线、格式骨架与操作流见 `.agents/skills/write-notes-like-deepseek/SKILL.md`
- 分工：ADR（`docs/decisions/`）只管难逆转的外部决策；事故走 `docs/postmortems/`；规范治理走 openspec；其余非平凡决策归 notes
- 校验：`./scripts/notes.sh verify`（CI `notes` job 门禁）；检索：`rg --hidden <关键词> .agents/notes/`
- 动手重构/选型前先查 `rejected/` 与相关 class 目录，避免重走被否掉的老路

## 已知权衡

- CI（clang-18）与本地（clang-21）编译器版本刻意并存；统一触发条件与完整理由见
  `.agents/notes/implemented/process/2026-07-24-ci-local-clang-version-divergence.md`
