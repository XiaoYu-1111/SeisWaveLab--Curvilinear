# 文档导航

| 内容 | 入口 |
| --- | --- |
| 项目背景、方法概览、软件架构、界面、应用与参考文献 | [仓库首页说明](../README.md) |
| 不同主频四组实验、面波频散及反演恢复的可配置重绘 | [实验绘图 Notebook](../tools/seismic_plotting/README.md) |
| 二维/三维模型 VTS 复现与所选波场分量、记录导出 | [模型与波场数据交换](MODEL_AND_WAVEFIELD_IO.md) |
| 实际 DEM 预览、横/纵剖面、范围选择与 CSV 导出 | [DEM 剖面提取 Notebook](../tools/dem_profiles/README.md) |
| 控制方程、交错/同位网格比较、曲网格映射、DRP/RK 与稳定性 | [曲网格波场理论整理](CURVILINEAR_WAVE_THEORY.md) |
| 定向体力、各向同性爆炸源、单位与验证 | [震源机制](SOURCE_MECHANISMS.md) |
| 二维外部高程格式、分层弹窗预设与大模型容量 | [外部高程与模型容量](SURFACE_PROFILES.md) |
| 二维参数应用、共用记录显示与 SEG-Y 导入/分量导出 | [地震记录与 SEG-Y](SEGY_AND_RECORDS.md) |
| 主页维度入口、二维 P–SV 原理及独立源码分组 | [二维与三维工作流](TWO_D_WORKFLOW.md) |
| 背景、原理、实现过程、界面介绍与附录 | [完整学术说明](PROJECT_MANUAL.md) |
| 方程、边界闭合、差分、时间组合与诊断 | [数值原理](NUMERICAL_METHOD.md) |
| 模型、震源、接收器、显示与快捷键 | [用户指南](USER_GUIDE.md) |
| 12 种主题、磨砂材质、左侧 / 悬浮 / 并存参数面板 | [主题与悬浮参数面板](APPEARANCE_AND_PANELS.md) |
| 黄土塬干扰波、采集参数初筛与当前能力边界 | [采集生产应用分析](ACQUISITION_PRODUCTION_VALUE.md) |
| 接收几何及 CPU/CUDA 计时 | [采集与性能](ACQUISITION_AND_PERFORMANCE.md) |
| 二维/三维三档 CPU/CUDA 实测、倍速曲线与原始数据 | [性能对比报告（2026-10-19）](PERFORMANCE_BENCHMARK.md) |
| 分层介质与图标资源 | [层状模型](LAYERED_MODELS.md) |
| VS、依赖、外部输出、设备选择与分发 | [构建说明](BUILDING.md) |
| Git 上传内容及仓库边界 | [仓库组织](REPOSITORY_LAYOUT.md) |
| 已验证事项与证据边界 | [数值验证状态](NUMERICS_AND_VALIDATION.md) |

正文图片全部保存在本仓库 `docs/images`，GitHub 阅读不依赖外层 out。`images/capture.tsv` 保存已发表截图的算例与时间步元数据，供图集及复核使用。原始 PPM、模拟场和分析中间数据留在外层 `../out`。旧版本验证记录中的有效证据已合并到 [当前验证说明](NUMERICS_AND_VALIDATION.md)，仅保留最新的 [0.16 专项验证](VERIFICATION_0_16.md)；性能实测、曲线和原始数据随 [性能报告](PERFORMANCE_BENCHMARK.md) 保存在仓库内。

## 生成离线说明和博客

从仓库根目录执行 `python -m pip install -r tools/requirements.txt`，然后执行：

```powershell
python -B tools/package_documentation.py
```

工具直接读取仓库内正文、发表 PNG 和截图元数据，无需重新模拟。离线图文包生成到 `../out/documentation`，GitHub Pages / Hexo 两套资源生成到 `../out/blog`，ZIP 也在对应外层目录。

两版博客同源，只适配 front matter 和资源路径。Hexo 保持 `![说明](01-model.png)`，PNG 放在文章同名资源目录。已验证的 NexT / kramed 兼容修复见[Hexo 数学公式设置](HEXO_MATH.md)。打包工具不修改网站或推送。

当前截图全部保留原生 2560×1494 完整客户区。三维重新采集用 `--capture-workflow`，二维用 `--capture-2d`，主页用 `--smoke-home`；二维完整流程位于 `../out/screenshots/two-d-workflow`。三维输出位于 `../out/screenshots/curvilinear-workflow`；`python -B tools/analyze_dispersion.py` 输出位于 `../out/validation/dispersion`。采集后先复核图像、参数及解释，再运行 `python -B tools/publish_screenshots.py` 无损转换到 `docs/images`，最后重新打包。不要复制原始帧、构建日志或模拟中间文件进入 docs。
