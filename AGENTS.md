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
压缩（未压缩普通文件）：
  parser×K 字节块切分 + 边界对齐 --[MPMC]--> encoder×N (2-bit + zstd×3) --[MPMC]-->
  writer (ChunkOrderer 保序 → 帧头 + 写盘)

压缩（gzip / stdin / 双端）：单 reader 顺序解析，encoder/writer 同上
解压：reader(readRawFrame) --[MPMC]--> decoder×N --[MPMC]--> writer(reorder + 滚动校验和)
```

核心模块：
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
- 新代码遵循上述策略；不要顺手翻写既有注释（避免无价值 diff）。

## Git

- 提交信息使用中文
- 格式：`<类型>: <简述>`，如 `refactor: 移除 Quill 依赖`、`feat: 添加 SPSC 并发流水线`

## 复盘

非平凡问题（竞态、难复现 bug、架构权衡变更）写复盘，不入 CHANGELOG 正文。
- 目录：`docs/postmortems/`，索引与约定见 `docs/postmortems/README.md`，模板见 `TEMPLATE.md`
- 文件名：`YYYY-MM-DD-slug.md`，七节：症状 / 复现 / 调查 / 根因 / 修复 / 验证 / 后续与教训
- CHANGELOG 对应"修复"条目末尾加 `→ 详见 docs/postmortems/...`

## 已知权衡

### CI 与本地 clang 版本差异（2026-07）

- CI（`.github/workflows/ci.yml`）用 clang-18 + 内联 `-s compiler.version=18`
- 本地 conan profile 用 clang-21
- 差异影响有限：项目未使用 21 独有的 C++23 特性（`<print>`、ranges 补全等），libc++ 18/21 在已用特性上行为一致
- 暂不统一：升级 CI 到 21 需引入 apt.llvm.org 源，镜像变重；降本地到 18 放弃 23 改进。当前权衡可接受
- 触发升级的条件：项目开始用 21 独有特性，或 sanitizer 行为出现跨版本差异
