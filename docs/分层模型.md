# 层状模型与图标（0.8）

## 提供的黄土剖面

“介质”页选择“黄土五层（提供参数）”，参数严格使用用户表格：

| 地层 | 厚度 m | Vp m/s | Vs m/s | 密度 kg/m³ |
|---|---:|---:|---:|---:|
| 干黄土 | 5 | 550 | 250 | 1550 |
| 湿／压实黄土 | 5 | 800 | 350 | 1800 |
| 黏土／古土壤 | 20 | 1500 | 700 | 2050 |
| 砂岩 | 20 | 2500 | 1300 | 2250 |
| 硬岩基底 | 自动 | 4000 | 2300 | 2650 |

末层以厚度 0 表示自动填满剩余深度。支持 2..8 层，可编辑有限层厚度、各层 Vp、Vs、密度，增加/删除基岩前的一层。“展开地层编辑表”可在较大浮层中同时查看全部列。所有物理修改暂存，点击“应用修改并重建”才生效并清零记录；非法层数据保留旧会话。

## 消除同一网格层的横向跳点

默认分层坐标为 **按网格层对齐（同 k 同物性）**。按物理网格零基索引 k 的计算深度 `d=k×h` 选层，因此同一个 k 平面的全部 X/Y 节点具有完全相同的 Vp、Vs、密度和地层编号。界面随曲网格层连续变化，深部网格展平后地层界面也随之展平。

该模式中的层厚是计算坐标的名义厚度；实际物理厚度随曲网格伸缩，不再强制各水平位置到当地地表的竖直厚度完全相等。这是避免地形导致某一列提前跨层、邻列延后跨层而产生跳格的关键。

另保留两种显式物理深度模式：

- 地表以下物理深度：`d=z−H(x,y)`，各列具有相同的竖直埋深分层。
- 物理基准面 Z：`d=z`，以统一基准面的水平界面分层。

后两种模式的物理界面通常不与曲网格节点层重合，逐节点赋值仍可能呈现台阶；选择它们代表优先保持物理深度定义。默认模式不靠图像模糊掩盖台阶，而是从材料赋值规则上确保同 k 同物性，CPU、CUDA、显示及导出共用相同规则。

有限层的下边界累计为 `b_i=Σ thickness_j`，选取满足 `d<b_i` 的第一层；恰好位于界面上的节点归入下层。当前不对界面做平滑混合，也没有宣称接触界面的反射误差已通过解析验证。

## 网格与时间设置

原默认 h=10 m 无法分辨 5 m 薄层。按钮 **“暂存分层算例：1 m／完整时长”** 设置：100×20×100、平地、h=1 m、12 Hz、时长 0.40 s、最大步数 0、居中震源 Z=3；应用后重建。两层 5 m 薄层各对应五个计算单元，名义深度到 99 m，基岩填充 50 m 之后部分。

全局启动最大步数现为 5000、时长 1 s、CFL=0.8；上述完整算例按钮暂存最大步数 0。最大步数可能使计算早于模拟时长结束，应查看运行页的实际终止时间和 Ricker 主峰时刻。总步数由当前网格、最大 Vp、吸收阻尼与主频计算，旧 CFL=0.22 的约一万余步不能当作新版固定步数；宜按机器性能选择 CPU 或 CUDA。

诊断采用全部配置地层的最大 Vp 约束 CFL、最小 Vs 估计波长节点数，最薄层单元数和缺失/不足两格的地层数另行显示。模型深度不足以包含基岩时也提示。五格只是薄层的基础几何分辨率，仍需结合主频、曲网格拉伸和界面反射误差做精度检查。

## 其他预设

以下为可编辑的常用结构示例，数值为测试设置，不代表现场标定值：

| 预设 | 各层（厚度 m；Vp/Vs m/s；密度 kg/m³） |
|---|---|
| 低速覆盖层／基岩 | 15；800/350；1800 → 自动；2500/1300；2300 |
| 风化岩三层 | 8；900/400；1900 → 17；2000/1000；2250 → 自动；4000/2300；2650 |
| 沉积四层 | 5；650/280；1700 → 10；1400/600；2000 → 25；2600/1400；2350 → 自动；4500/2600；2700 |

几何尺寸预设仍在“模型”页，介质预设在“介质”页；两者分开，避免选择物性时无意改变几何。均匀弹性介质仍可使用。

## 参与计算、显示与导出

层状模式将逐节点 λ、μ、ρ 传给 CPU/OpenMP 与 CUDA。动量方程除以当地密度，应力方程使用当地 Lamé 常数，自由表面局部闭合和机械能也使用当地物性。高斯源按当地密度作加速度归一化。材料不是仅用于渲染的贴图。

显示分量增加“介质 Vp”“介质 Vs”“介质密度”，二维和三维切片显示实际生效材料。材料色标从 0 到该材料的配置最大值，不受波场显示增益改变。VTS 的 Vp、Vs、Density 输出各节点材料值，层状模式额外输出从零开始的 LayerId；原有波场数组保留。

当前空间算子是同位中心差分与局部系数的异质速度—应力形式。材料跳变处不声称保持内部光滑场的四阶收敛，需要网格加密、反射/透射标准算例进一步量化。

## 程序图标

通过内置 imagegen 生成“曲面地形＋分层地质块＋冷暖地震波环”的图形，再用 Pillow 转为 Windows 多尺寸 ICO；图标绑定 EXE 资源 101，并设置窗口大小图标。

- PNG 原图：`resources/icons/seis3d.png`（相对于仓库根目录）。
- ICO：`resources/icons/seis3d.ico`，包含 16、20、24、32、40、48、64、128、256 像素。
- 64 px 预览：`resources/icons/preview64.png`。
- 资源模板：`resources/AppIcon.rc.in`；独立仓库入口将 RC 生成到外层 `../out/build/<预设>/generated/AppIcon.rc`。

图标原件作为应用资源随源码提交，生成的 RC、对象文件与 EXE 在外层 out。清理 out 后仍可使用仓库内 ICO 构建完整图标。原图生成时使用的提示词记录如下：

> Use case: logo-brand. Asset type: Windows desktop application icon for Seis3D Curvilinear, a scientific 3D elastic seismic wave simulator. Create one polished square icon, no text or letters. A bold compact isometric geological block with gently curved topographic surface, three broad visible strata on front faces, and a single luminous cyan and warm coral seismic wave arc propagating across its top. Dark deep teal rounded-square tile, crisp layered turquoise, blue and muted sandstone geometry. Strong simple silhouette, broad shapes and very few lines, legible when reduced to 32 pixels. Flat dimensional scientific design with restrained highlights, centered composition filling 85 percent of square, no thin grid, no charts, no badges, no watermark, no surrounding mockup. Background outside the rounded tile transparent. Save a production-ready square PNG with alpha.
