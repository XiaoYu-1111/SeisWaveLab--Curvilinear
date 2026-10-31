# 地震波场实验重绘 Notebook

入口是三个Notebook和`run.py`。绘图算法放在同目录`seisplot/`中，15Hz/30Hz共用代码。所有当前配色及显示参数集中在[统一样式配置](styles/current_publication.json)，详细约定见[参数与色标说明](参数与色标说明.md)。

## 《石油地球物理勘探》投稿图件

保留现有 `style_profile` 及其数据、配色和色标设置，在 JSON 或 Notebook 参数中另外加入：

```python
CONFIG['journal_layout'] = 'ogp_double'  # 通栏；单栏用 'ogp_single'
CONFIG['formats'] = ['png', 'pdf', 'svg']
CONFIG['records']['individual_panels'] = True  # 可选：另存每个道集子图
```

`ogp_single` 和 `ogp_double` 的目标宽度分别为 81.5 mm 和 170 mm；按缩放后的物理尺寸重排文字，字号限制为 7～9 pt，位图以 600 dpi 导出，PDF/SVG 保留支持的矢量对象。此项仅控制显示版式与输出，原始数组、色标上下限、插值和增益策略不变。每张图仍需检查刻度、单位、标注、面板布局及打印尺寸，过密的多子图应拆分；具体分辨率和字体须以期刊最新投稿须知为准。单张数据图及分图应与论文编号、图注一起检查。解析后的具体参数与样式版本写入 `data/config_used.json`。

道集设置 `individual_panels=True` 时，在四模型组合图之外逐一导出对应单栏图；各分图复用组合图的显示数组和色限，不对单幅图重新归一化。其他类型图件仍需依据实验和期刊要求分别准备分图。

从高程 CSV 生成单栏图的示例（基准高程和震源坐标均须由实际实验指定）：

```powershell
python -B tools/seismic_plotting/plot_elevation.py --profile ../out/jingbian_15Hz_2m/surface_profile.csv --datum-m 1481.322148135886 --source-x-m 5000 --output-dir out/papers/投稿图件/高程剖面
```

横坐标和高程从原 CSV 读取，标记为震源的地表投影；图件及输入文件哈希、显示参数写入外层 `out`。该图不重算或修改地形。

| Notebook | 输入 | 输出 |
| --- | --- | --- |
| [01 不同主频四组模型对比](01_不同主频四组模型对比.ipynb) | 一个或多个主频的四组SEG-Y、模型VTS、快照索引 | 原始/RMS/AGC记录、模型及浅层、同时刻波场、RMS/频谱/镜像/晚时/偏移距、反射参考和主频对比 |
| [02 面波波场与频散对比](02_面波波场与频散对比.ipynb) | 面波实验算例目录、可选理论曲线CSV | 记录、模型、裁切波场、网格/边界残差、原始道叠加、频散谱、多偏移距窗口和无理论门限的谱峰CSV |
| [03 频散反演恢复对比](03_频散反演恢复对比.ipynb) | 已有反演结果CSV、拟合曲线CSV、正演真值和起始模型参数 | 模型恢复、逐层偏差、曲线拟合、多初值、层厚扫描和敏感性图 |

## 使用

1. 在VS Code或JupyterLab中打开Notebook，选择装有依赖的Python内核。
2. 修改开头的参数单元格。常规重绘只需改输入目录/文件、输出目录；更换模型、道集布局或分层数时也应检查相应参数。
3. 从上向下执行。耗时的全模型/波场读取可通过`RUN_MODELS`、`RUN_WAVEFIELDS`关闭。
4. 图件在输出目录的`figures/`，统计和实际配置在`data/`，可失效缓存位于`cache/`。不会写回原始模拟文件。

默认分别写到项目外层`out/replots/四组实验_最新`、`out/replots/surface_wave`、`out/replots/inversion`。输出必须是本工作区外层`out`的子目录；可任意选择其下的文件夹。代码会拒绝向仓库、输入数据根目录或其他位置写图，遵循仓库的输出布局规则。同名输出可能被重写；新默认目录与历史0–200 Hz成果分开。

Notebook会自动从当前目录向上寻找Git仓库。若从无关目录启动，在第一个代码单元格填写`REPOSITORY_ROOT`。相对输入路径以Git仓库的父目录（外层工作区）为基准，输入也支持绝对路径，包括位于其他磁盘的数据。没有硬编码`D:`或本机用户名。

依赖见[requirements.txt](requirements.txt)。在自己的环境安装后运行：

```powershell
python -m pip install -r tools/seismic_plotting/requirements.txt
python -m jupyter lab tools/seismic_plotting
```

已有环境无需重新安装。微软雅黑不可用时会明确提示并选择已安装的字体；为中文图件建议安装微软雅黑或Noto Sans CJK。

## 不用Notebook也能重绘

在Git仓库根目录执行，`-B`禁止生成仓库内的Python字节码：

```powershell
python -B tools/seismic_plotting/run.py --kind four --config tools/seismic_plotting/examples/four_frequency.json
python -B tools/seismic_plotting/run.py --kind masw --config tools/seismic_plotting/examples/surface_wave.json
python -B tools/seismic_plotting/run.py --kind inversion --config tools/seismic_plotting/examples/inversion.json
```

可以只执行部分步骤：

```powershell
python -B tools/seismic_plotting/run.py --kind four --config tools/seismic_plotting/examples/four_frequency.json --steps inspect,records,metrics,frequencies
python -B tools/seismic_plotting/run.py --kind masw --config tools/seismic_plotting/examples/surface_wave.json --steps inspect,dispersion,windows,picks
```

查看统一样式、预览实际参数或只重绘一个主频：

```powershell
python -B tools/seismic_plotting/run.py --list-styles
python -B tools/seismic_plotting/run.py --kind four --config tools/seismic_plotting/examples/four_frequency.json --frequency 15 --check-config
python -B tools/seismic_plotting/run.py --kind four --config tools/seismic_plotting/examples/four_frequency.json --frequency 30 --steps records,metrics
```

`--frequency`输出到`out/replots/四组实验_最新/15Hz`或`30Hz`，避免单频执行互相覆盖清单；不筛选时两种主频写入同一批次。`--output-dir`也可覆盖为外层out内的其他子目录。

JSON示例是小型版本化配置，原始SEG-Y、VTS、缓存和旧PPT没有复制进Git。克隆仓库后需自行放置数据，或改为实际输入路径。`out/tools`中的10个旧绘图/补丁/后处理入口已改为转发统一工具，默认写入`out/replots`，不覆盖正式PPT；旧脚本原文和哈希归档在`out/maintenance/绘图代码整理_20261027`。历史Analyze脚本仅供数值溯源，新的常规绘图不调用。面波旧脚本保留实验来源，以本目录Notebook为常规维护入口。

## 输入目录或独立文件

默认每个算例采用程序原生导出结构：

```text
算例目录/
  run_metadata.json
  model/model.vts
  records/Vx.sgy
  records/Vz.sgy
  records/receiver_geometry.csv
  snapshots/times.csv
  snapshots/wavefield_1.vts ...
```

文件名不同时，在Notebook参数单元格覆盖算例配置即可。例如只读一个Vz分量：

```python
c = CONFIG["datasets"][0]["cases"][0]
c["folder"] = r"E:\my_experiment\model_A"
c["record_files"] = {"Vz": r"E:\my_data\vertical.sgy"}
c["geometry_csv"] = r"E:\my_data\receivers.csv"
c["model_vts"] = r"E:\my_data\velocity.vts"
c["snapshot_index_csv"] = r"E:\my_data\times.csv"
c["snapshot_dir"] = r"E:\my_data\snapshots"
# 没有metadata文件时，按真实参数提供，不猜测震源或网格间距。
c["metadata"] = {"source_x_m": 200, "source_depth_m": 1,
                 "spacing_m": 0.25, "frequency_hz": 20}
```

不画模型或快照时，只需提供要使用的SEG-Y与几何。不同算例可提供不同组件。分量必须匹配的对比步骤会明确报错。`snapshot_index_csv`列为`file,target_time_s,actual_time_s,step`，按目标时间匹配，不按文件排序冒充同时刻。索引中的`file`相对于`snapshot_dir`。

SEG-Y从卷头/道头读取dt、样本格式、道数、每道点数，支持IBM32（格式1）、IEEE32（5）、IEEE64（6）及已知数量的扩展文本头。卷/道头dt冲突、截断记录、变道长和非有限样值会报错。头中缺少dt时必须显式提供`sample_interval_s`；显式值覆盖有效头值会给出警告。角度坐标、缺失或重复X坐标应提供几何CSV。

几何CSV每道一行，必需`x_m`，可选`z_m`、`offset_m`。缺少offset时使用`x_m-source_x_m`。不假定250/241道、6001/5001点或固定0.5ms。不同道数和采样的算例可分别画图；残差分析要求dt、道数/点数、偏移距一致，不会静默重采样。

VTS目前支持本程序的**二维X-Z ASCII StructuredGrid**，读取`Vp`、`Vs`、`Vx`、`Vz`及`Coordinates`等命名数组。不支持二进制/压缩VTS和三维体网格，会明确报错。单行大数组按块解析，显示抽稀后缓存，并保留原生全分辨率最值。缓存键包含源路径、文件尺寸、修改时间、字段及抽稀步长，更改输入文件后不会复用旧缓存。若外部程序刻意保留了文件尺寸和修改时间，应手动清除**输出目录**的缓存。

## 常用显示参数

公共参数在`styles/current_publication.json`，数据路径与实验专有参数在`examples/*.json`。优先级：公共样式 → 实验样式 → JSON/Notebook显式覆盖。每次输出保存解析后的配置及样式SHA-256。也可以直接在Notebook内覆盖：

```python
CONFIG["formats"] = ["png", "pdf"]
CONFIG["dpi"] = 200
CONFIG["snapshots"]["times_s"] = [0.5, 1.0]  # 必须是已有导出时刻
CONFIG["snapshots"]["crop_m"] = [4800, 5200, 0, 160]
CONFIG["snapshots"]["relative_x"] = False
CONFIG["snapshots"]["depth_coordinate"] = "eta"
CONFIG["snapshots"]["equal_aspect"] = True
```

- 输入、裁切和空间计算统一使用m。道集可通过`records.distance_unit`选择m/km，仅转换坐标显示。`records.velocity_unit`可选m/s或um/s；仅原始记录转换速度单位，RMS/AGC仍为无量纲。空间1:1通过`equal_aspect`选择，默认15/30Hz宽幅比较不强制1:1。
- `depth_coordinate="physical"`读取真实保存的基准深度Z；`"eta"`为原生网格行号乘`spacing_m`的计算坐标；`"surface_relative"`为物理Z减当地地表Z。后二者在曲网格中不能混用。
- `relative_x=True`时裁切X以震源为零，否则为绝对X。`crop_m=[xmin,xmax,zmin,zmax]`只裁图，不修改模拟域。
- 模型默认turbo，波场coolwarm，记录gray、双线性数据阶段显示插值。负黑、零灰、正白。规则间距记录用imshow，不规则接收X使用pcolormesh保留真实位置，不插造缺失道。原始DEM继续使用jet；频散相干谱viridis、Qp试验模型plasma，各自具有不同物理意义。
- 波场采用对称对数。15/30Hz配置固定所有模型、主频和9时刻共用±20µm/s，线性核±0.2µm/s；不同PNG可在这一共同显示尺度下观察响应，定量仍读取原始样本。`scale_scope="fixed"`需正的`vmax_m_s`；`"global"`无固定上限时扫描同一数据集的所有所选帧；`"per_time"`无固定上限时逐时刻自动定标，不可由颜色直接判断跨时刻衰减。显式`vmax_m_s`始终优先。全局扫描保留原生极值，渲染逐时刻读取，不把全部波场与坐标数组一起常驻内存。
- `stride_x/z`只对显示数据抽稀，不改变物理坐标和原始文件。裁切较细节的图应减小步长。
- `snapshots.individual=True`时，除模型对比图外，另存每个算例每个时刻的单独快照，仍使用该对比图的同一物理色阶。
- 四组实验AGC窗口300ms，原dt=0.5ms时为600样点；边缘按实际可用样点归一化，分母下限为同一模型/分量整体RMS的1%，显示±3。整体RMS以分析道上速度平方的时间积分除时长计算。面波保留reflect边缘、奇数窗口、最大局部RMS的3%下限，不强套四组设置。增益只用于显示。
- 四组实验默认输出每分量四模型矩阵及原始Vz/Vx并列图（左Vz、右Vx）；无低速层对±10µm/s，有低速层对±125µm/s，15/30Hz相同。单分量四模型原始矩阵共用±125µm/s。波场只输出Vz，默认全部9个已导出时刻，0–10km全宽。
- `reference_depths_m=[1000,2000]`仅在物理基准深度坐标绘制细灰虚线；η/相对地表深度图不标这两个物理界面。右侧标注带浅底，避免遮挡波场。

## 反射与频散结果的解释

反射图的层界和Vp在`reflection_reference`显式提供，子波延迟默认1.5/主频，可用数据集`source_delay_s`覆盖。更换震源子波后必须检查延迟约定。起伏地表使用的是几何近似参考，并非严格曲面射线追踪。红/橙线不是自动拾取。相关性计算始终使用原始记录。

频散计算使用指定分量的原始记录、Tukey端点渐消、逐道相位归一化和相移叠加。理论CSV只叠加显示，不用于限制峰值搜索。全局相干峰仍可能包含高阶模态、近场或混叠，**不能直接当作已确认基阶**。时窗和孔径决定实际分辨率，细化搜索网格不增加观测信息。理论列名可通过`theory_frequency_column`和`theory_mode_columns`配置。

## 反演Notebook的CSV格式

默认结果表一行一个模型：

```text
id,success,rmse,h1,h2,h3,vs1,vs2,vs3,vs4
model_A,1,0.3,10,20,40,250,520,800,1250
```

最后一层为半空间，没有其厚度。`results_csv`可以是一个或多个文件；结果ID不得重复。新表头可通过`id_column`、`rmse_column`、`vs_columns`、`thickness_columns`映射。缺少厚度列时使用配置中的固定层厚。`fit_csv`默认列：

```text
frequency_hz,observed_m_s,initial_m_s,fitted_m_s
```

可用`fit_columns={"frequency":...,"observed":...,"initial":...,"fitted":...}`映射；`initial`列可省略。拟合曲线RMSE与所选结果行不符时会报错，避免选错文件。理论曲线可选。这里不自动解析Qt所有历史私有文本格式，必要时先整理为上述CSV。

`truth`为正演真值，`initial_vs_m_s`为反演起始模型，二者分开配置；`free_vs`明确哪些层真正参与求解。灰底和误差表标出固定先验，不把其零偏差计作独立恢复。`multistart_groups`、`thickness_scan`和`sensitivity_cases`可省略，或填写新的模型ID。本Notebook只画已有反演结果，不编译Qt、不运行优化、不改原始模型。

## 来源与验证

重构来源：`Analyze-Jingbian-Records[-30Hz].py`、`Export-Jingbian-Gain[-30Hz].py`、`Export-Jingbian-Vz[-30Hz].py`、`Revise-Jingbian-Figures[-30Hz].py`、面波`analyze_results.py`和`plot_inversion.py`。保留物理单位、原始幅度定量、AGC显示分离、真实网格、裁切和模态限制；移除固定工作盘、固定样本数、缓存目录依赖及字符串补丁。

PPT专用全原生网格投影/同步脚本保留在外层out的复现工程，用于重建既有报告图件；现已读取同一统一样式。常规维护优先本目录。Q对照分析工具亦复用这份配色表，但保留其校准窗口、幅度标定和AGC算法。

本工具包不自动构建PPT。重绘PNG/PDF可替换到后续报告。统计方法和显示配置会随每次输出保存，适合复核。原始旧报告的全部硬编码数值或解释没有复制为新算例的结论。

运行读入及数值检查（夹具与日志生成到外层out）：

```powershell
python -B -m unittest discover -s tools/seismic_plotting/tests -v
python -B tools/seismic_plotting/check_notebooks.py
# 本机已有正式15/30Hz图件时核对清单、18张波场哈希、界面线与显示参数：
python -B tools/seismic_plotting/verify_publication.py
# 在默认实验数据齐全时，执行三个Notebook的实际数据检查：
python -B tools/seismic_plotting/check_notebooks.py --execute
```

版本库中的Notebook清除所有执行输出。保留带输出副本时另存到外层out；不要把缓存、图件或原始数据提交进Git。

## 多道频谱与平均谱

Notebook 01在道集之后输出15/30 Hz各自的Vz、Vx四模型振幅频谱。汇报图横轴为0–100 Hz，线性振幅m/s，全部接收道以彩色细线叠加，平均频谱为黑色粗线。每道全时窗去均值、加Hann窗、单边FFT，除窗和修正幅度，正频率乘2（DC/Nyquist除外）。逐道取模值后进行道算术平均；不先平均波形，不施加AGC或逐道归一化，不剔除边缘道。各模型纵轴按实际幅度适配，比较幅值时需读取刻度。原metrics步骤的矢量Welch功率谱保持原口径，本图为线性振幅谱。

可用`--steps spectra`单独运行。`trace_spectra.display_max_hz`只控制图面上限，默认100 Hz；旧配置`max_hz`作为显示上限兼容。`window`可选`hann`或`boxcar`。逐道及平均谱CSV保存原始频率分辨率下完整单边频谱（最高可用FFT频点不超过Nyquist频率），单位m/s；`trace_spectra_panels.json`仅存图面频段，`trace_spectra_policy.csv`同时记录显示与数据上限。0–100 Hz不是滤波。历史0–200 Hz数据仍在`out/replots/four_frequency`，正式汇报图在`out/replots/报告频谱_0-100Hz`，普通重绘不覆盖正式PPT。

## Q值模型图

04报告的Qp/Qs四组全域和浅层图使用`out/presentations/03_复现工程/04_Q值实验/build/plot-q-models.py`。它读取四个启用Q工况的VTS及metadata，核验源文件哈希，并按求解器的计算坐标与分层参数重建Q值；全域用真实基准深度Z，浅层显示地表下物理深度，统一plasma 0–200。它不适用于任意其他实验，参数为未标定试验值。输出PNG、PDF和来源JSON，默认写在脚本旁的`q_models`；可用`--output-dir`指定外层out内另一目录。完整命令、数据路径和复现边界见[绘图脚本使用与复现](../../../out/presentations/02_说明文档/绘图脚本使用与复现.md)。
