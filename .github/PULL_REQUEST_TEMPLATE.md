## 变更摘要

<!-- 这个 PR 做了什么,为什么 -->

## 类型

- [ ] 修复(bug fix)
- [ ] 新增(feature)
- [ ] 重构(refactor,无行为变化)
- [ ] 文档 / CI / 构建
- [ ] 格式兼容性改动

## 验证

- [ ] `./scripts/lint.sh format-check` 通过
- [ ] `./scripts/test.sh clang-debug` 全绿
- [ ] `./scripts/test.sh clang-asan` 通过(C++ 改动必选)
- [ ] `./scripts/test.sh clang-tsan` 通过(并发相关改动)
- [ ] clang-tidy 无告警
- [ ] CHANGELOG 已更新

<!-- 如涉及性能/内存/归档行为,附上实测数字 -->

## 格式兼容性

<!-- 是否影响 .fqc 线上格式?若影响,说明 openspec 变更与冻结 fixture 的更新情况 -->

- [ ] 不影响现有归档(解码/字节布局不变)
- [ ] 影响线上格式,已走 openspec 变更流程并更新 `tests/fixtures/`

## 相关链接

<!-- 关联的 issue / openspec change / postmortem -->
