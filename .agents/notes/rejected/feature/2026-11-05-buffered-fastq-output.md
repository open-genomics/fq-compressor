# Agent Note: 解压侧 4 MiB FASTQ 输出缓冲

Status: rejected — 端到端无可测收益，反而多一次拷贝与 4 MiB 常驻缓冲

## Problem

`writeFastqRecord` 每条记录多次 `operator<<`，直觉上可以先把一帧记录拼进大缓冲，
再用一次 `write` 落盘，减少 stream sentry/streambuf 调用。

## Proposal

在 `ArchiveEngine::decompress` 增加 4 MiB `std::vector<char>` 缓冲，逐记录
`append`，达到阈值或帧末 `write`。超大单记录回退到直接写。

## Alternatives considered

- **保持现有 `operator<<`**：`std::ofstream` 已有内部缓冲；逐字段写虽然调用多，
  但每次写入量小且没有额外大缓冲。实测 baseline 并不慢。
- **改用 `std::streambuf::sputn` / 自定义 `std::streambuf`**：可能减少 sentry
  开销，但仍要经过 streambuf 缓冲；复杂度高于收益。
- **只对 stdout 启用大缓冲**：stdout 场景更可能受逐字符锁影响，但当前 CLI 主要
  落盘；没有独立证据前不增加分支。

## Acceptance criteria

- 解压输出 `cmp` 一致；
- realistic 64 MiB、5 次交错 A/B 的解压 wall/user CPU 中位数提升 ≥5%；
- 峰值 RSS 不明显增加。

## Risks

实测 59 MiB realistic 解压：当前 0.133–0.140 s vs baseline 0.134–0.137 s，
差异在 A/A 噪声（约 5%）内且方向不稳定；4 MiB 缓冲还增加常驻内存和一次拷贝。
未达到门槛，回退，不纳入默认路径。若未来 stdout/管道成为主场景，可单独重测。
