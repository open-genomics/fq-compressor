# FQC v2 设计文档站（GitHub Pages）

fq-compressor 的 GitHub Pages 站点，用一页介绍三大主题：

1. **压缩格式设计** —— `fqc-sequential/v2` 归档布局、完整性、治理
2. **压缩算法设计** —— 列式分离、2-bit 打包、质量直通、校验与 varint
3. **软件高性能架构设计** —— 并行流水线、内存模型、Profile、故障行为

内容改写自仓库根目录的 `ALGORITHM.md`、`ARCHITECTURE.md` 与
`openspec/specs/archive-format/spec.md`，站点地址：
<https://open-genomics.github.io/fq-compressor/>

## 结构

```
docs/pages/
├── index.html            # 单页站点（侧栏 + 三大主题 + 资源）
├── assets/
│   ├── style.css         # 深色主题样式
│   ├── app.js            # 滚动高亮 + 移动端抽屉导航
│   ├── format-layout.svg         # 图 1 · 归档字节布局
│   ├── columnar.svg              # 图 2 · 列式分离
│   ├── pipeline-compress.svg     # 图 3 · 压缩流水线
│   ├── pipeline-decompress.svg   # 图 4 · 解压流水线
│   └── memory-model.svg          # 图 5 · 内存模型
```

纯静态、零构建、零外部依赖；所有资源用**相对路径**引用，
因此对任意 base path（如项目页 `/fq-compressor/`）都能直接工作。

## 部署

- 工作流：[`.github/workflows/pages.yml`](../../.github/workflows/pages.yml)
  在推送 `docs/pages/**` 时构建并部署。
- **仓库要求**：Settings → Pages → Source 选择 **GitHub Actions**。
- 生成站点 `<https://open-genomics.github.io/fq-compressor/>`。

## 本地预览

```bash
cd docs/pages
python3 -m http.server 8000
# 打开 http://localhost:8000/
```

## 维护要点

- 内容与根目录 `ALGORITHM.md` / `ARCHITECTURE.md` / openspec 保持同步。
- SVG 图使用主题色（`#38bdf8` 青 / `#34d399` 绿 / `#fbbf24` 琥珀），
  与 `style.css` 中的 CSS 变量保持一致。
- 新增章节时，在 `index.html` 的侧栏 `<nav>` 与对应 `<section>` 各加一项，
  并给 `<section>` 标注 `data-cat`（`format` / `algorithm` / `architecture` / `overview`）
  以启用滚动高亮。
