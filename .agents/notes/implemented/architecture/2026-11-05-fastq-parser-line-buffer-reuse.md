# Agent Note: FastqParser 行缓冲复用

Status: implemented

## Problem

`FastqParser::readRecord` 每条记录都会新建 `idLine` / `plusLine` 两个
`std::string`。它们只在解析当前记录时使用，却会在每条记录上触发短命分配；
header 和 `+` 行通常很短，分配本身比内容处理更显眼。

## Decision

在 `FastqParser` 中增加 `idLine_` / `plusLine_` 成员，`readRecord` 复用它们。
parser 由单 reader/worker 线程独占，不需要加锁；复用只影响解析期临时缓冲，
不影响 `ReadRecord` 的拥有数据。

## Alternatives considered

- **完整缓冲扫描器 / SoA 解析**：收益更大，但会把解析器重写成另一套架构，
  超出“简单清晰”的范围。
- **只复用 `plusLine`**：`+` 行几乎总是极短，单独复用收益更小，代码也对称性更差。
- **让调用方传入 scratch buffer**：会把 parser 的内部细节暴露给所有调用方，
  增加 API 复杂度；parser 自己持有更自然。

## Consequences

- 每条记录少两次临时字符串构造/析构；`ReadRecord` 的 id/comment/sequence/quality
  仍按原语义分配，行为不变。
- `FastqParser` 不再是“无状态解析器”，但它是单线程对象，这个状态边界清晰；
  文档注释说明复用原因和线程归属。
- 归档字节不变：59 MiB realistic 输入下新旧二进制 `.fqc` `cmp` 一致。

## Verification

- `clang-debug` 构建 + `./scripts/test.sh clang-debug`：17/17 通过。
- `clang-asan` / `clang-tsan`：17/17 通过。
- 59 MiB realistic、1 MiB frame、4 parser：新旧二进制 `.fqc` 逐字节一致；
  解压输出与原 FASTQ 一致。
