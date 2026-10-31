# 文档导航

2026-10-31正演采集更新：声学P1/DRP与弹性/黏弹性支持随模型自动沿地表均匀布设检波器，期望/实际道数分别保存；可切回手动，模型VTS保留设置，旧模型采集线保持兼容。[操作与验证](FORWARD_SURFACE_RECEIVERS_2D.md)。

2026-10-31 RTM交互更新：新增顶部及固定底部重建按钮，接收起点/道数/间隔/深度设置，默认随模型修改自动沿地表均匀布设；旧成像结果与新模型分开显示，支持再次运行。[操作、截图与验证](RTM_2D.md)。

2026-10-31高阶声学升级：新增独立CPU/CUDA、FP32/FP64曲网格DRP7–RK4试验核，含零压力自由面、外扩ADE-CPML、震源正则化及选择性数值耗散。解析/界面/吸收与八项回归通过；15/30 Hz局部对照未通过5%等误差门槛，P1继续默认，RTM保持P1。[实现、实测与下一阶段门槛](ACOUSTIC_DRP_2D.md)。

2026-10-31补充：声学默认自动RAM预算，切换CPU/CUDA及精度立即刷新容量，修复重建几何内存重复计数；真实界面CUDA FP32 5000×2000完成8步容量验收。新增“采集 → 地表均匀布设”，并补充有限元与高阶差分的取舍。[说明与验证](ACOUSTIC_CAPACITY_AND_METHOD.md)。

2026-10-31更新：二维功能工具栏分组留白；声学与弹性共用地形/分层编辑，按空闲内存与预算提示网格上限；压力增加SEG-Y rev2 IEEE32/64导出；RTM新增限速过程预览。[操作、截图与验证](WORKFLOW_UPGRADE_2D.md)。

2026-10-31：二维弹性/黏弹性及声学吸收区都可叠加真实波场，保留原浅橙色，新增“边界着色不透明度”（默认0.14）。[操作、截图与验证](ABSORBER_DISPLAY_2D.md)。

新增：[二维声学CPML、模型波场叠加与记录面板验收](ACOUSTIC_CPML_2D.md)。包含六组吸收对照、原始CSV、精度/容量和复核命令。

| 内容 | 入口 |
| --- | --- |
| 声学/RTM任务分组、菜单箭头、统一布局、独立输入及核心依赖 | [二维模块组织与隔离](MODULE_ARCHITECTURE_2D.md) |
| 声学/弹性/黏弹性切换、P波压力正演、大网格容量与模式选择 | [二维三种正演模式](ACOUSTIC_FORWARD_2D.md) |
| 独立二维成像入口、声学 RTM、源照明、检查点与实测验收 | [二维 RTM 与源照明](RTM_2D.md) |
| 黄土沟谷表观衰减先导、固定物理源核、紧凑通量探针与验收队列 | [先导实验实现](LOESS_SURFACE_ATTENUATION_PILOT.md) |
| 项目背景、方法概览、软件架构、界面、应用与参考文献 | [仓库首页说明](../README.md) |
| 不同主频四组实验、面波频散及反演恢复的可配置重绘 | [实验绘图 Notebook](../tools/seismic_plotting/README.md) |
| 布局、后台计算、材质缓存、复现和采集评价 | [优化进展与后续工作](OPTIMIZATION_PROGRESS.md) |
| 二维 Qp/Qs、论文目录、解析验证与受控模拟 | [二维黏弹性与复现](QP_QS_VISCOELASTIC_2D.md) |
| 二维 CPU/CUDA CFS-PML、Qp/Qs 联用与七组边界对照 | [二维吸收边界升级](CFS_PML_2D.md) |
| 二维模型/状态FP32与FP64、干黄土误差、速度和5001×2001容量 | [二维计算精度实测](PRECISION_2D.md) |
| 二维四档规模、CPU32线程／GPU性能模式、FP32/FP64、功耗及显存 | [最新二维性能报告（2026-10-29）](PERFORMANCE_PRECISION_2D.md) |
| 默认地表、VTI/VTS 导入、动态预算和高程配色 | [三维外部 DEM](EXTERNAL_DEM_3D.md) |
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
| 历史二维/三维三档FP64 CPU/CUDA实测与原始数据 | [历史性能对比（2026-10-19）](PERFORMANCE_BENCHMARK.md) |
| 分层介质与图标资源 | [层状模型](LAYERED_MODELS.md) |
| VS、依赖、外部输出、设备选择与分发 | [构建说明](BUILDING.md) |
| 可运行便携包、完整源码包、相对依赖与 GitHub Release | [Release 发布与重建](RELEASE_DISTRIBUTION.md) |
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

原有二维/三维工作流的34张界面图于2026-10-27重新采集，统一红黄橙主题，原生2560×1494完整窗口；二维采用参数/波场/记录并列布局，三维记录页扩大显示高度，默认DEM补充turbo空间图与俯视图。完整列表见[界面图集](SCREENSHOT_GALLERY.md)。性能与数值分析曲线保留原始数据来源和实验日期。重新采集入口为`--capture-workflow`、`--capture-2d`、`--smoke-home`；复核后使用`tools/publish_screenshots.py`无损发布，再重新打包。

2026-10-31更新弹性/黏弹性、声学和RTM界面，共十八张1600×1000原生完整窗口图，仍使用红黄橙主题；资源与像素校验见[模块界面发布清单](data/module-ui-2026-10-30/screenshots.json)。
