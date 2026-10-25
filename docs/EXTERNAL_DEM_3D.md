# 三维外部 DEM 地表：VTI / VTS

在“建模 → 外部 DEM 地表高程”选择 `.vti` 或 `.vts`，读取为待应用参数，核对原始数组、实际求解网格、间距和深度后应用 / 重建。地表入口仅保留 VTI / VTS。读取或参数预检失败时保留当前有效会话；重建资源回收与恢复规则见下节。

## 重建内存与会话恢复

界面重建先校验参数、源接收几何、DEM 覆盖范围及展平深度，再释放旧 CPU 网格 / 波场、CUDA 缓冲区和显示缓存，最后分配新模型；新旧完整体网格不会同时常驻。“重建可用 RAM / VRAM”、适配可用内存和 DEM 网格分配计入当前模型可回收的实际资源；切换 GPU 时只计入目标设备上的可回收显存，保留原有安全余量与总点数限制。

预检失败保持原会话。若释放后新模型创建失败，尝试按原参数和后端恢复到零时刻，并提示旧波场及接收记录已释放；若原模型也无法恢复，则尝试建立小型 CPU 会话供调整参数。直接调用底层 `initialize` 仍保留原有事务语义；界面使用 `rebuildSimulation3D` 处理资源回收。

`curvilinear_3d_rebuild_memory` 在 CPU / CUDA 上分别执行 6 次同尺寸、放大与缩小 DEM 重建，检查常驻资源无增长、分配顺序、无效参数保留会话、释放后失败恢复及继续推进。记录位于外层 `out/validation/rebuild-memory/resident.csv`。

## Notebook 导出

`tools/dem_profiles/DEM高程剖面提取.ipynb` 第 4 节为一维高程剖面，第 9 节为独立二维区域。二维面板支持拖框、精确长宽、按点数或间距采样，导出格式仅提供 VTI + VTS、VTI、VTS。默认 1 km × 1 km、300 × 300 点，不含末端。

文件名包含点数与采样间隔：`surface_grid_500x400_h2m.vti` / `.vts`；对应 CSV、JSON、PNG/PDF/SVG 使用相同前缀，统一存放在外层 `out/dem_profiles/regions/region_时间戳`。名称间距显示最多 12 位有效数字，真实间距按 VTK / JSON 的完整数值读取。

1 km 取 500 点是 2 m 采样，样点为 0,2,…,998 m，节点跨度为 998 m；包含终点时，1 km / 2 m 为 501 点。选区尺寸和节点跨度分别记录，不追加短间隔。加密采样不增加源 DEM 实测分辨率。

## 文件语义

| 格式 | 几何与高程 |
| --- | --- |
| VTI ImageData | 单层规则 XY 图像；Origin 为源平面首节点，Spacing 为真实采样间距；Elevation 点标量为实际高程 |
| VTS StructuredGrid | 单层曲面；Points 的 XYZ 为源平面坐标和向上为正的实际高程；Elevation 与点 Z 一致 |

点按 X 最快、Y 递增排序。VTI 在 ParaView 中可按 Elevation 使用 Warp By Scalar（比例 1、法向 +Z）；VTS 可直接显示起伏地表。[格式依据：VTK XML 规范](https://docs.vtk.org/en/latest/vtk_file_formats/vtkxml_file_format.html)。

两者包含 FieldData（高程基准与间距保留完整精度）：`ElevationDatum_m`、`HorizontalSpacing_m`、`HorizontalUnitMetres=1`、`ElevationUnitMetres=1`、`ElevationPositiveUp=1`。求解器局部 X/Y 从首节点起算，地表深度 `Z=基准−高程`。不含基准字段的外部 VTK 文件取高程均值；坐标系未知时不假设 EPSG。

## 支持范围与拒绝条件

- 当前导入支持 ASCII Float64 / Float32、单 Piece、WholeExtent 与 Piece 一致、Z 仅一层；VTI 需要 Elevation 或活动标量，VTS 允许由 Points Z 读取高程。
- XY 需平面米制、方格等间距，实际高程单位为米；VTS 检查每个节点的 XY 规则性及 Z / Elevation 一致性，并将递减行列归一化为 X/Y 增大。
- 不接受 binary/appended/压缩数组、体模型、旋转 VTI、歪斜或非等距网格、非有限值、空洞和超预算数组。不得直接将经纬度当米导入。
- 每轴 2..4096，总高程数不超过 4,000,000。求解网格按内存 / 显存、地形深度与总点数动态分配 XYZ，含幽灵层预算 28,652,616；导入数组与求解网格点数分别显示。
- 外部地表尚未内嵌到可复现模型体 VTS，界面会阻止遗漏地表的模型保存；保留地表 VTI/VTS 与模拟参数。波场 VTS 和接收记录仍可导出。地表 VTS 与模型体 VTS 使用不同入口。

## 方向与高程着色

显示坐标转换为 `(X,-Z,-Y)`，DEM 俯视与 X-Y 切片统一 X 向右、Y 向上；X-Z / Y-Z 保持深度向下。三维顶部“高程着色”复用波场完整的 14 种色带选项及同一 RGB 映射函数，包括冷暖渐变、Viridis、Turbo、Jet、地形配色、CUDA Green 等；高程可独立选择色带，以实际求解地表的最低 / 最高高程自动映射，并显示米制高程图例。切换无需重建，波场增益不改变高程色标。旧三色带首选项自动迁移，保留原颜色。点击“DEM 俯视”可适配窗口，方向指示器显示 +X / +Y / Up。

## 验证

`seis3d_surface_grid_validation` 检查 VTI/VTS 往返、原点 / 间距 / 高程基准、方向、错误输入保留会话与 CPU/CUDA 一致性。`seis3d_surface_budget_validation` 检查大数组和 XYZ 动态预算。

`SeisWaveLab_Curvilinear.exe --smoke-terrain [地表.vti 或 地表.vts 的绝对路径]` 执行实际 OpenGL 像素读回，检查全部 14 种共用色带、四个非对称位置、地表方向与关闭开关。图件位于外层 `out/validation/terrain-display`。`seis3d_presentation_validation` 检查高程符号、平地、旧首选项迁移、同色带归一化值对应的 RGB 一致性及显示坐标。
