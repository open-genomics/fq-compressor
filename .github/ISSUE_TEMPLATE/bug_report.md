---
name: Bug 报告
about: 报告一个可以复现的问题
title: "[bug] "
labels: bug
assignees: ''
---

<!-- 安全漏洞请勿用本模板，改走 SECURITY.md 的私有渠道 -->

## 问题描述

清晰简洁地描述遇到的问题。

## 复现步骤

1. 准备输入:<!-- 文件类型/来源,或生成命令 -->
2. 运行命令:
   ```bash
   fqc compress -i ... -o ...
   ```
3. 观察到:<!-- 实际行为 -->

## 期望行为

<!-- 预期应该发生什么 -->

## 环境

- **fqc 版本**:`fqc --version` 输出
- **构建方式**:release 二进制 / 源码构建(clang-debug / clang-release / 其他)
- **操作系统**:Linux/macOS/WSL2,发行版与内核
- **编译器**:clang 版本、libc++ 版本(如适用)

## 归档/输入信息

- 是否可提供输入文件或其前缀切片(脱敏后)?
- 归档疑似损坏时:`.fqc` 由本实现还是 Rust `fq-compressor-rust` 生成?

## 其他上下文

<!-- 日志、栈、`verify` 输出、相关 issue 等 -->
