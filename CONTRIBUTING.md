# 贡献指南

感谢你愿意为 fq-compressor 贡献。项目定位是**内存有界、管道友好、格式可校验**的 FASTQ
归档工具（C++23）。在提交代码前，请先阅读本指南与 [AGENTS.md](AGENTS.md)（构建/测试/代码风格约定）。

参与即表示你同意遵守 [行为准则](CODE_OF_CONDUCT.md)。

## 快速开始

```bash
git clone https://github.com/open-genomics/fq-compressor.git
cd fq-compressor
./scripts/build.sh clang-debug      # 构建
./scripts/test.sh clang-debug       # 运行全部测试
./scripts/lint.sh format-check      # 格式检查
```

工具链要求与质量门禁详见 [docs/building.md](docs/building.md)。

## 提交前检查清单

| 改动类型 | 必做检查 |
|---|---|
| 任何代码改动 | `./scripts/lint.sh format-check` + `./scripts/test.sh clang-debug` |
| C++ 代码 | 追加 `./scripts/test.sh clang-asan`（ASan+UBSan 门禁） |
| 并发/线程相关 | 追加 `./scripts/test.sh clang-tsan`（无数据竞争） |
| 格式/归档布局改动 | 必须通过 openspec 变更流程并更新冻结 fixture（见下） |
| 用户可见改动 | 更新 [CHANGELOG.md](CHANGELOG.md)（新增/修复/变更段） |

## 提交信息规范

- 使用**中文**撰写提交信息。
- 格式：`<类型>: <简述>`，例如 `feat: 添加 XX`、`fix: 修复 XX`、`refactor: 重构 XX`、
  `docs: 更新 XX`、`test: 增加 XX 单测`、`ci: 调整 XX 工作流`、`chore(openspec): 归档 XX`。
- 一个提交只做一件事；关联 openspec 变更时在提交信息中注明变更 ID。

## 注释与文档语言策略

- **公开头文件 API 注释用英文**（`include/fqc/**` 下的 Doxygen 风格 `///` 文档），
  面向所有读者；描述接口契约、不变量与线程安全语义。
- **实现内注释用中文**（`.cpp` 内的行内/块注释），解释"为什么"与取舍，而非复述代码。
- 文档（README、docs/、CHANGELOG、openspec）用中文。
- 新代码遵循上述策略；不要顺手翻写既有注释（避免无价值 diff）。

## 代码风格

- C++23，clang + libc++，4 空格缩进，100 列限制。命名、错误处理、日志约定见
  [AGENTS.md](AGENTS.md)「代码风格」节。
- 库代码（`src/`、`include/fqc/`）**不用异常**，错误经 `Result<T>`
  （`std::expected<T, Error>` 别名）传播。
- 静态分析：`clang-tidy`（`.clang-tidy` 配置，CI 门禁，`WarningsAsErrors`）不得有告警。

## 格式兼容性（重要）

`.fqc` 归档格式由 `openspec/specs/archive-format/` 规范约束，并与 Rust 实现
`fq-compressor-rust` 的 `fqc-indexed/v2` 族通过 magic 区分。任何影响线上字节布局的改动：

1. 走 openspec 变更流程（`openspec/changes/`，见 [openspec/AGENTS.md](openspec/AGENTS.md)）。
2. 冻结 fixture（`tests/fixtures/sequential-v2/`）是解码兼容契约：改动格式时需重新生成并
   更新 MANIFEST；禁止为兼容旧 fixture 而悄悄放宽解码校验。
3. 保证 `--parse-workers 0` 与 `1` 归档逐字节一致（分帧是纯内容函数）。

## 报告问题

- 优先用 [Issue 模板](https://github.com/open-genomics/fq-compressor/issues/new/choose)
  提交（bug 请附复现步骤、环境与 `fqc --version`）。
- 安全漏洞**不要**发公开 issue，请走 [SECURITY.md](SECURITY.md) 的私有渠道。
- 不确定是否该修/该加？先开 issue 讨论，避免大 PR 方向跑偏。

## PR 流程

1. 从 `master` 切出功能分支；本地通过全部检查清单。
2. 提 PR 时填写 [PR 模板](.github/PULL_REQUEST_TEMPLATE.md)。
3. 改动会触发 CI：build-and-test（clang-debug）、sanitizer（ASan+UBSan）、coverage、
   clang-tidy、format-check 全部绿才可合入。
4. 合入方式由维护者决定（通常 squash merge）。

## 发布

版本号语义化（`VERSION` 文件）；发布时维护者把 CHANGELOG「未发布」段固化为版本段、
打 `vX.Y.Z` tag，由 `release.yml` 自动构建并附二进制到 GitHub Release。
