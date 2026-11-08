# Agent Note: FASTQ 行读取改为固定缓冲分块扫描

Status: implemented

## Problem

`FastqParser::readLine` 原先用 `std::getline(std::string&)` 读完整行后才检查
`maxLineBytes_`。gzip 解压出的恶意超长行会先按整行长度分配内存，绕过
`--memory-limit` 把进程打满。

改成固定缓冲后还暴露了一个 EOF 语义问题：`std::getline(char*, n)` 在
"读到 EOF 但已取到字符"时只置 `eofbit`，不置 `failbit`。用 `!fail()` 判断
"遇到分隔符"会把这种情况当成分隔符并减掉一个字符，导致**没有尾随换行的
最后一行被截断**，合法记录被拒绝。

## Decision

- 每次 `getline` 使用 64 KiB 栈缓冲；只有累计内容不超过 `maxLineBytes_`
  才追加到 `line`，超限立即 `ignore` 排空当前行并返回格式错误。
- 分隔符判定改为 `!fail() && !eof()`；缓冲区满（`failbit`）时 `clear()`
  后继续读下一块。
- `bytesConsumed_` 按 `gcount()` 累计原始字节，超长行错误路径也不会漏计。

## Verification

- `FastqParserTest.RejectsLineLongerThanLimit` /
  `AcceptsLineAtLimitAcrossChunkBoundary`：跨 64 KiB 块的拒绝与接受。
- `FastqParserTest.AcceptsFinalRecordWithoutTrailingNewline`：回归修复前会被
  截掉最后一个 quality 字符。
- `./scripts/test.sh clang-debug`：19/19 通过。

## Alternatives considered

- **继续用 `getline(std::string&)` + 事后检查**：无法避免先分配整行，恶意输入
  仍可打满内存。
- **改用 `std::istream::get()` 逐字符读**：实现简单，但每次虚函数调用，热路径
  吞吐明显下降；分块 `getline` 保留批量读取。
- **只加大 `maxLineBytes_` 上限**：治标不治本，仍按行分配。

## Consequences

- 单行内存上界为 `maxLineBytes_` + 64 KiB，与 read 长度无关。
- 无尾随换行的最后一行按原样接受，行为与旧实现一致。
