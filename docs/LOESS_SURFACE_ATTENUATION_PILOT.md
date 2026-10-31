# 黄土沟谷表观衰减先导：实现、参数与启动方式

## 2026-10-29结果检查

本轮完成4个研究左炮和11个数值资格算例后，在全尺度网格验收处停止，剩余10个研究先导未运行。0.5 m与0.25 m的原始Vz记录L2差约15.5%–23.1%，高于原5%门槛；同一对照的15 Hz幅度差中位数约2.3%–3.9%，不能据此代替全波形验收。扩域/PML对照的L2残差约6.71×10⁻⁸，当前主要问题是网格波形一致性。0.25 m仍是加密参考，尚未证明最终收敛。

检查和重绘入口为[review_loess_pilot.py](../tools/review_loess_pilot.py)。本地检查结果在`../out/research/loess-apparent-attenuation/pilot_20261029_02/review_20261029`，包括输入完整性核对、分频/走时诊断、600 dpi PNG/PDF、HTML图集和Markdown报告。按当前显示约定采用模型turbo、记录gray、波场coolwarm+detail_symlog；波场所有模型与时刻共享全局尺度，模型/波场保持物理深宽比例。AGC仅用于显示，不参与Q或原始振幅评价。

更新：2026-10-29。此页说明先导入口与验收队列，不把启动、软件回归通过或局部基准完成写作科学结论。详细研究设计和运行数据位于本地外层 `out/research_design/loess_scattering_q_20261029`、`out/research/loess-apparent-attenuation`，不属于源码包。

## 研究与参数

研究地形诱导的面波振幅变化是否会被误判为材料Qs，采用平地/沟谷、弹性/GSLS、双向激发的受控对照。主方案800×200 m、0.5 m、15 Hz、源η深度2 m、竖直线力100 N/m；两炮x=150/650 m，401道位于x=200–600 m，1 m水平道距，Vx/Vz、0.5 ms、完整3 s/6001点。外扩40 m CFS-PML、FP64、三机制GSLS拟合3–45 Hz、参考15 Hz。

层厚10/20/30/剩余m，Vp为550/800/1500/2500 m/s，Vs为250/350/700/1300 m/s，密度1550/1800/2050/2250 kg/m³。基准Qp/Qs为40/25、80/50、120/75、200/125；Qs=50组只改变第一层Qp/Qs为80/50。参数是等效假设，不是特定工区实测剖面。

先导14组：F/H20 × E/Q25 × L/R共8组；均匀半空间F/H20 × L/R共4组；F/Q50 × L/R共2组。后续完整42组研究不在此启动命令的自动范围内。

## 两项核心扩展

### 固定物理源核

`Parameters::sourceSigmaM`、`sourceRadiusM`同时为零时，完全保留原来的5×5格点源核。非零时，只支持定向体力，在实际物理坐标内计算圆形截断高斯核，以Jacobian加权归一化；原有推进中的h²因子使总二维线力不随网格变化。本先导σ=0.5 m、半径1.5 m。

不允许负值、非有限值、半径小于σ、物理核与矩源混用或核触及计算域边界。诊断记录构建时的权重和及RMS物理半径。0.5/0.25 m平地测试的RMS半径约0.689160/0.688775 m；CPU/CUDA FP64波场相对L2约4.9×10⁻¹⁶。上述是限定测试的结果，不是所有地形的传播精度保证。

启用物理核时，模型使用二维schema 6，在原元数据与校验中保存`PhysicalSourceKernelM`，并显式保存精度和吸收设置；旧schema 1–5继续读取为原源核。旧版可执行程序不能读取schema 6，需要使用更新构建。当前界面载入后保留该参数，尚无专用源宽度编辑控件；切换为矩源前需恢复零/零源核设置。

### 紧凑探针

`Simulation::readProbes`接收逻辑模型节点索引，返回Vx、Vz、Sxx、Szz、Sxz。CPU直接取样；CUDA用节点索引聚集内核只回传指定点，不下载完整网格。支持FP32/FP64，转换为double输出不会提高原FP32有效精度。非法索引拒绝并保留原输出，空列表返回空；读取不改变波场或接收记录。

先导在x=300/500 m、η=0–80 m及η=80 m的x=300–500 m三条线上，每约1 ms取样；文件同时标明请求与实际时间。保存各点物理位置、外法向、梯形线积分权重及原始五场。输出带符号机械功率通量W/m，不把控制面净通量差直接称为黏弹性热耗散。

## CMake目标与验证

源码入口：[LoessPilot2D.cpp](../src/two_d/LoessPilot2D.cpp)、[专用验证](../src/two_d/LoessPilotValidation2D.cpp)、[Python队列](../tools/run_loess_pilot.py)。

```powershell
# 在Git仓库根目录的MSVC开发者终端执行。
cmake --preset x64-Release
cmake --build --preset x64-Release --target seis2d_loess_pilot seis2d_loess_pilot_validation --parallel 4
ctest --preset x64-Release -R curvilinear_2d_loess_pilot
```

2026-10-29相关7项回归通过：新源核/探针、GSLS、二维模型IO、二维物理/后端、CFS-PML、外扩吸收、FP32/FP64。专用检查包含schema 6两精度往返、CPU/CUDA探针与完整快照逐值比较、非法索引、空探针及源宽度加密检查。完整论文的面波与复杂层系验证由下面的数值队列继续执行。

## 有限队列与停止规则

准备前需保留本地设计包中的`terrain_*.csv`及GSLS支路文件。此脚本不会联网下载或重新生成这些研究输入。相同输出目录存在内容时拒绝替换。

```powershell
python -B tools/run_loess_pilot.py --prepare --root ../out/research/loess-apparent-attenuation/SESSION
python -B tools/run_loess_pilot.py --run --root ../out/research/loess-apparent-attenuation/SESSION
```

`SESSION`由使用者选定一个新的目录名。首先只初始化预检：CPU/GPU资源、几何、源核、Q、时间步和完整时长；然后依次运行：

1. 平坦均匀半空间E/Q25 × h=1/0.5/0.25 m，6组、1.2 s，与独立复Rayleigh方程参照比较。
2. 先导中的4个左炮F/H20 × E/Q25，完整3 s。
3. 对应4个0.25 m参考域，2.4 s，核对共同0–2.4 s数据；另跑H20/Q25左炮的300 m深、60 mPML对照，完整3 s。
4. 网格与边界门槛通过后，继续剩余10个先导，最终科学先导总数14。另有11个数值资格算例，不混计为研究样本。

局部基准检查15 Hz附近的相速度、衰减和网格变化；全域资格同时检查原始记录L2、15 Hz振幅差和扩域残差。各项量及具体候选门槛在`analysis/uniform_gate.json`和`full_domain_gate.json`中输出。不通过时停止，不通过静默改参数、降低门槛或删除异常算例继续。

0.25 m参考只到2.4 s，因此2.4–3 s晚时尾波没有获得这项网格资格证据；发表前还应逐频、逐窗验证α、相速度和交互残差。通过队列门槛不等于完整科学精度认证。初始14组通过后再决定是否开展非对称地形与盲测。

## 输出与溯源

- 每个算例：配置、模型VTS、Vx/Vz原始Float64 CSV、兼容IEEE32 SEG-Y并逐样本回读、几何、12时刻Vx/Vz快照、紧凑探针及状态。细网格和局部验证使用各自的时长/快照表，不冒充12帧正式先导。
- 探针`fields_f64.bin`：8字节`SWP5F64\n`标识、uint32小端节点数；每帧float64实际时刻，随后`[节点,5]`个小端float64。空间和时间定义见`geometry.csv`、`index.csv`。
- `flux.csv`为三条控制线的带符号向外功率；上下文、单位和积分权重必须与原始探针一起使用。
- `COMPLETE.txt`只表示完整时长、有限场、快照、模型往返与SEG-Y回读通过。每个输出随后生成SHA-256清单。
- 根目录`plan.json`冻结配置、目标程序摘要、源码摘要及队列顺序；`source`保留相关源码副本和当时工作树差异。`batch_status.json`含控制器/子进程PID、当前算例、实际时间、步数与完成计数。

运行时只占用一张GPU、一个模拟器；低于20 GiB磁盘余量时在下个算例前停止。控制器在运行期间请求系统不自动睡眠，允许屏幕熄灭；结束或失败后撤销该请求。关闭控制器不是断点保存；已有输出不会自动覆盖或拼接成完整结果。
