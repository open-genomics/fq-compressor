# 真实语料验收

并发课程 A–H 收束后的关账测量：用两份公开 FASTQ 前缀切片验证 round-trip，并复测阶段 I 的质量流门槛。切片不入库，用 `scripts/fetch_real_corpus.sh` 重建。

环境：AMD Ryzen 7 5800H（WSL2），Clang 18.1.3 Release，默认 16 GiB 内存预算，`/proc/uptime` 计时。吞吐按未压缩 FASTQ 字节 / 墙钟。压缩各 3 次，取中位数；其余步骤各 1 次。WSL2 波动大，数字只用来下结论，不替代阶段 E 的 A/B 平台。

## 切片

| 切片 | 来源 | 仪器 / 策略 | 记录 | 碱基 | 文件 | SHA-256 |
|---|---|---|---:|---:|---:|---|
| `SRR2962693_1.head200k.fastq` | [SRR2962693](https://www.ebi.ac.uk/ena/browser/view/SRR2962693) R1 前 200k 条 | Illumina WXS，126 bp 定长 | 200 000 | 25.20 Mbp | 53.96 MiB | `6e38214ac7ebe0c2ba981a616672f695606d456d9dd1066f80f546109039ad9c` |
| `DRR171398_1.head4k.fastq` | [DRR171398](https://www.ebi.ac.uk/ena/browser/view/DRR171398) 前 4k 条 | 人类 MinION | 4 000 | 65.36 Mbp | 124.77 MiB | `748c3d34cdd31996e7ddb7472cd7d941d6744fc4df5d131df33e9ce07e643f1e` |

质量分布（本切片）：

- Illumina：Q 均值 35.89、中位 38，24 个质量符号（偏集中）。
- ONT：读长 288–108 205 bp（均值 16.3 kbp），Q 均值 35.93、中位 33，**93 个质量符号**（近满 Phred 字母表）。

重建：

```bash
./scripts/fetch_real_corpus.sh
```

## 正确性

| 切片 | auto profile | 命令 | verify | decompress `cmp` |
|---|---|---|---|---|
| Illumina | `illumina` | 默认并行解析 | 通过 | 与输入逐字节一致 |
| ONT | `ont`（ENA run accession + 长读） | 默认并行解析 | 通过 | 与输入逐字节一致 |

`--quality-level 7` 的归档同样 `verify` 通过。并行与顺序路径的归档不必逐字节相同（K>1 时块尾关帧）；记录顺序与内容必须相同，本轮 `cmp` 已确认。

## 压缩比与吞吐

| 切片 | 压缩比 L1 | 压缩 L1 | 顺序解析 | 相对顺序 | 解压 | verify |
|---|---:|---:|---:|---:|---:|---:|
| Illumina WXS | **4.15×** | 87.0 MiB/s | 28.1 MiB/s | **+210%** | 150 MiB/s | 225 MiB/s |
| 人类 MinION | **1.96×** | 33.5 MiB/s | 27.1 MiB/s | +24% | 65 MiB/s | 121 MiB/s |

对照合成随机数据（阶段 H，64 MiB 预算）：Illumina-like 2.96× / 149 MiB/s，ONT-like 2.84×。

解读：

1. **短读真实质量更肥。** Illumina 切片 4.15×，明显高于随机合成的 2.96×——质量符号只有 24 个且集中在 Q38 附近，zstd 吃得到。`ALGORITHM.md` 里「真实数据通常优于随机合成」在短读上成立。
2. **这条 ONT 切片更瘦。** 1.96× 低于合成 2.84×。质量流近满字母表，通用压缩器吃不到条件熵；长读序列的 2-bit 打包收益被高质量值字节摊薄。阶段 I 把质量流标成压缩比主瓶颈，在真实长读上比合成更明显。
3. **并行解析在真实短读上仍然值钱。** 本切片 +210% 大于合成短读的 +47.7%（真实 ID/质量行更贵，Amdahl 里 parse 占比更高）。长读仍是小头（+24%）。

## 质量流门槛复测

门槛不变：体积改善 ≥3% **且** 压缩吞吐回退 ≤10%，才改默认 zstd level。

| 切片 | L7 体积相对 L1 | L7 吞吐 | 吞吐回退 | 过门槛？ |
|---|---:|---:|---:|---|
| Illumina WXS | −1.19% | 28.0 MiB/s | −68% | 否 |
| 人类 MinION | **+0.01%**（更大） | 24.0 MiB/s | −28% | 否 |

**默认保持 level 1。** 阶段 I 在拟真/随机质量上的结论，换公开真实切片仍然成立。`--quality-level` 继续只作实验入口。

## 发现：ENA 风格 ONT 头行

DRR171398 经 ENA 转写后的头行是 `@DRR171398.1 1/1`，没有 `runid=` 或 channel 标签。关账时 auto profile 会拒绝；随后已把「长读 + INSDC run accession」收进 `detectProfile`，本切片现可 auto 判为 `ont`。短读 ENA 头（如 `@SRR2962693.1`）仍走长度规则，判为 Illumina。

ENA 转写也会抹掉 PacBio 的 `/ccs` 等标记。这类长读现在会标成 `ont`。当前所有 profile 共用回退编码，标签不影响压缩；若要归档元数据准确，PacBio 的 ENA FASTQ 仍应传 `--profile`。无 accession、无平台标记的长读继续拒绝猜测。

## 结论

- 闭环成立：两份切片 compress → verify → decompress → `cmp` 全过。
- 压缩比天花板仍在质量流；真实短读比合成好看，真实长读可以更难看。
- 没有档位因此改默认 codec。
- 并发课程 A–H 可以收束；下一步若还做，应是新课题，而不是阶段 J。
