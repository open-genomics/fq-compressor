# Conan 工具链漂移（zlib-ng/Clang21/CMake export）

- 日期：2026-07-13
- 严重度：medium
- 状态：closed
- 引入点：v2 phase 6-7（pre-75b7400）
- 相关：`conanfile.py`、`cmake/FQCompressorConfig.cmake.in`、`CMakePresets.json`

## 症状

三个相互关联的依赖/工具链问题阻碍了干净的 release 构建与外部消费方使用：

1. zlib-ng 默认模式未导出 `ZLIB::ZLIB`，并允许静默回退到系统 zlib。
2. Clang 21 比 Conan 声明的 compiler 设置更新，导致 ASan 配置被拒绝。
3. 安装后的 CMake export 将 `COMPONENT` 解释为 include 路径，破坏了外部消费方。

## 复现

1. 全新 `clang-release` 配置，zlib-ng 处于默认模式——结果解析到系统 zlib 而非 Conan 的 zlib-ng。
2. `./scripts/test.sh clang-asan`，使用 Conan 自带的 Clang 兼容性设置——被拒绝。
3. 外部项目 `find_package(FQCompressor)` + `target_link_libraries(... FQCompressor::fqc_core)`——include 路径错误。

## 调查

- zlib-ng：确认 compat 模式下别名 target 缺失；系统 zlib 静默满足了该依赖。
- Clang 21：Conan 的 profile 只识别到 Clang 20；`compiler.version` 检查拒绝了 21。
- CMake export：`COMPONENT` 参数被放在了 CMake 将其解释为路径片段的位置。

## 根因

- zlib-ng 需要 `zlib_compat=True` 才能暴露 `ZLIB::ZLIB` target。
- Conan 的兼容性表落后于已安装的编译器；该设置必须钳制到已知最高值，而非留给自动检测。
- install/export 的参数放置对 `COMPONENT` 关键字是错误的。

## 修复

- 在 `conanfile.py` 中设置 `zlib_compat=True`；全新配置现在解析到 Conan 的 zlib-ng target。
- 检测已安装的编译器，仅将 Conan 的兼容性设置钳制到其已知最高 Clang 值（20）；每次依赖安装后删除未使用的根 `CMakeUserPresets.json` 聚合器，避免多个 `conan-debug` preset 冲突。
- 纠正 install/export 的参数放置，并验证外部消费方能配置/构建/运行。

## 验证

- 全新 `clang-release` 与 `clang-asan` 配置/构建通过。
- 外部消费方项目针对已安装的包完成配置、构建并运行。

## 后续与教训

`install_deps.sh` 流程现在每个 preset 产出单一工具链；外部消费方检查已纳入 `README.md` 的发布清单（release checklist）。
