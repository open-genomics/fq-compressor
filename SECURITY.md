# 安全政策

## 支持的版本

| 版本 | 支持状态 |
|---|---|
| `v0.1.x` | ✅ 受支持 |
| 更早版本 | ❌ 不受支持 |

## 报告漏洞

请**不要**在公开 issue 中报告安全漏洞。选择以下渠道之一:

1. **GitHub 私有漏洞上报**(首选):仓库页面 → Settings → Security →
   [Private vulnerability reporting](https://github.com/open-genomics/fq-compressor/security/advisories/new)。
2. **邮件**:jiashuai.mail@gmail.com(邮件主题请以 `[SECURITY]` 开头)。

我们会在收到报告后尽快确认(通常 3 个工作日),并在修复发布前不公开细节。

## 本项目威胁模型说明

fq-compressor 的完整性模型是**随机损坏检测**,而非密码学认证:

- 归档校验基于 **XXH64**(三层:全局头、帧逻辑流、footer 滚动累积),用于发现截断、
  传输损坏、存储位翻转等**意外**错误。
- XXH64 **不是**密码学哈希,对**恶意**篡改不提供防伪保证。若归档可能被未授权方篡改,
  请在上层自行叠加认证机制(如对 `.fqc` 文件做签名或 AEAD 加密)。

## 缓解措施(内置)

- 所有长度/大小字段先校验再分配,内存超限 fail closed(`--memory-limit`,默认 16 GiB)。
- 未知 magic / 版本 / 编解码器一律拒绝,无部分兼容路径。
- 覆盖已有输出需显式 `--force`。
