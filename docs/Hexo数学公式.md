# Hexo / NexT 数学公式渲染

本配置于 2026-10-18 在 Hexo 8.1.1、NexT 8.27.0、Node.js 22.15.0 下验证。

## 原因

站点原用 `hexo-renderer-kramed 0.1.4`，将独立公式输出为 `<script type="math/tex; mode=display">`，这是 MathJax 2 的旧接口；当前 NexT 的前端引擎为 MathJax 3，不能按原配置识别这些标签。主题和文章已启用 `mathjax: true`，仅重复启用该开关不能修复输出格式不兼容。

Markdown 中的 LaTeX 公式属于扩展功能，需要相配套的解析器和数学排版引擎。本次改用 Markdown-it 与构建时 MathJax SVG 输出，保留源文中的 `$...$`、`$$...$$`、下标、矩阵、aligned 和显式 tag 编号，避免 Markdown 先处理公式符号。

## 配置

在博客根目录操作，先备份 `package.json`、`package-lock.json`、`_config.yml` 和已有 `_config.next.yml`。若有其他 Markdown 渲染器，同一扩展名只保留一套。

```powershell
npm uninstall hexo-renderer-kramed
npm install --save-exact hexo-renderer-markdown-it@7.1.1 markdown-it-mathjax3@5.2.0
```

合并到站点 `_config.yml`，已有同名节点应合并，不重复添加：

```yaml
markdown:
  render:
    html: true
    breaks: true
    linkify: true
  anchors:
    level: 1
  images:
    prepend_root: true
    post_asset: true
  plugins:
    - name: markdown-it-mathjax3
```

合并到博客根目录 `_config.next.yml`，关闭前端的重复处理；数学公式仍会在生成 HTML 时直接排版：

```yaml
math:
  every_page: false
  mathjax:
    enable: false
  katex:
    enable: false
```

文章可保留 `mathjax: true`。不要再添加另一套客户端 MathJax/KaTeX 脚本。原主题文件无需修改，根目录覆盖配置更便于维护。

```powershell
npx hexo clean
npx hexo generate
npx hexo server
```

本地预览确认后，按原站点发布流程部署。不要把博客源文章改成渲染后的 HTML；后续仍编辑普通 Markdown。

## 检查

本次站点完整生成成功。曲网格主文得到 122 个 `mjx-container`，没有 `merror` 或 `data-mjx-error`；旧 `math/tex` 标签及重复的前端 MathJax 加载均已消除，30 个正文图片链接可解析。142 个原始文章及资源文件的 SHA-256 在修改前后相同。

验证渲染器时也应检查行内公式、独立公式、aligned、显式编号、表格以及代码块。反引号和围栏中的公式示例应保持代码，不要通过全局替换反斜线或下划线来修复排版。

配置、依赖锁文件和重新生成的站点需要共同保留。若线上页面仍是旧版，重新部署并刷新浏览器/CDN 缓存；本地重新生成不会自动更新 GitHub Pages。

参考：[NexT 数学公式设置](https://theme-next.js.org/docs/third-party-services/math-equations)、[Hexo 官方 Markdown-it 渲染器](https://github.com/hexojs/hexo-renderer-markdown-it)、[Markdown-it MathJax SVG 组件](https://github.com/tani/markdown-it-mathjax3)。
