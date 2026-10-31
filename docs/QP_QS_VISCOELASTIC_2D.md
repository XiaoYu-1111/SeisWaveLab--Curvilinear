# 二维 Qp/Qs 黏弹性：论文、实现与复现

2026-10-28：二维GSLS现支持FP32/FP64选择，模型系数、全部记忆及PML工作区同步选择精度；默认FP64，FP32模型保存schema 5。干黄土、弱尾波误差及中大网格容量见[精度实测](PRECISION_2D.md)。下列2026-10-25原实验保留其FP64证据口径。

更新：2026-10-25。当前模块支持二维平面应变 P-SV、CPU/OpenMP 与单卡 CUDA，默认关闭；三维仍为无显式 Q 的弹性求解。此次完成的是经典 GSLS 方法方程复现、独立解析基准和自定义受控算例，不是指定论文全部图件或现场工区的逐项复现。

## 论文目录与用途

可导入 Zotero / LaTeX 的[六篇文献 BibTeX](references/attenuation.bib)随仓库保存。

| 文献 | 在本研究中的用途 | 获取与阅读状态 |
| --- | --- | --- |
| Emmerich & Korn, 1987. *Incorporation of attenuation into time-domain computations of seismic wave fields*. Geophysics 52, 1252-1264. [DOI](https://doi.org/10.1190/1.1442386) | 时域衰减与松弛机制的经典来源 | 已核对书目信息；未取得出版社完整全文 |
| Robertsson, Blanch & Symes, 1994. *Viscoelastic finite-difference modeling*. Geophysics 59, 1444-1456. [DOI](https://doi.org/10.1190/1.1443701) | 速度-应力与记忆变量方法来源 | 已核对书目信息；Rice 1993 技术报告链接此次访问被拒，未作为已读全文 |
| Blanch, Robertsson & Symes, 1995. *Modeling of a constant Q: Methodology and algorithm for an efficient and optimally inexpensive viscoelastic technique*. Geophysics 60, 176-184. [DOI](https://doi.org/10.1190/1.1443744) | 有限频带近常 Q 与成本权衡 | 已核对书目信息和摘要；未宣称复现其全部拟合策略 |
| Hao & Greenhalgh, 2019. *The generalized standard-linear-solid model and the corresponding viscoacoustic wave equations revisited*. GJI 219, 1939-1947. [DOI](https://doi.org/10.1093/gji/ggz407)；[作者公开 PDF](https://xqihao.github.io/publication/hao-greenhalgh-2019/hao-greenhalgh-2019.pdf) | 复核复模量、松弛函数与微分方程的等价性及傅里叶符号 | 已查作者公开全文。采用等价的广义 Maxwell 表示，代码为独立实现 |
| Hu et al., 2023（卷号 2022）. *Modeling seismic wave propagation in the Loess Plateau using a viscoacoustic wave equation with explicitly expressed quality factor*. [开放全文](https://www.frontiersin.org/journals/earth-science/articles/10.3389/feart.2022.1069166/full) | 黄土塬、起伏地表、吸收和三维研究的应用参照 | 已查公开全文；该文是黏声波，不能当作本模块 Qs/面波验证 |
| 韩令贺等，2024.《鄂尔多斯盆地黄土塬区地震波场数值模拟及传播规律》. 石油地球物理勘探 59(3), 504-513. [期刊页面](https://html.rhhz.net/SYDQWLKT/2024-3-504.htm) | 激发位置、深度与近地表吸收的采集应用参照 | 已查公开页面；未复现其完整三维工区与实测资料对照 |

独立实现参考还包括 [Devito 黏弹性教程](https://www.devitoproject.org/examples/seismic/tutorials/09_viscoelastic.html)和 [SPECFEM 衰减参数约定](https://specfem3d.readthedocs.io/en/latest/04_creating_databases/)。这些用于核对物理定义，不代表两套程序已逐道对照。论文链接与书目随源码保留；未将版权受限全文重新分发。

## 当前方程与参数含义

保持现有曲网格的物理空间导数。动量方程仍为 `rho * dv_i/dt = d_j sigma_ij + f_i`。以压缩模量 `M=lambda+2mu` 和剪切模量 `mu` 构造广义 Maxwell/GSLS：

$$C(s)=C_R+\sum_l\Delta C_l\frac{s}{s+\omega_l},\quad C\in\{M,\mu\}.$$

采用 `s=i*omega` 的谐波约定，耗散模量虚部为正。与部分文献使用的负号约定相差复共轭，不改变衰减方向。时域总应力与记忆变量满足：

$$\dot\sigma=C_\infty:\dot\epsilon-\sum_l r_l,\qquad
\dot r_l=\omega_l(\Delta C_l:\dot\epsilon-r_l).$$

机制频率在拟合频带对数分布，使用小规模非负最小二乘拟合模量 `Q=Re(C)/Im(C)`。这是本实现的拟合策略，不照搬某篇论文的优化器。参考频率处按复波数的实部归一化，使输入 Vp、Vs 为该频率的**相速度**；改变参考频率时，不能把同一数值速度误认为整个频带都相同。

每个材料要求松弛体积模量为正、每个分支的 `DeltaM - 4*DeltaMu/3 >= 0`，以及剪切分支非负。独立指定 Qp/Qs 后，如果当前拟合违反这些被动性条件会拒绝应用；这不是把 Qkappa 直接设成 Qp。当前输入范围 Qp/Qs=5..1e9，机制数 1..3。关闭衰减是严格的原弹性路径；大 Q 是近似极限。

平面应变仍只传播 Vx/Vz，但额外积分 Syy，以 `p=-(Sxx+Syy+Szz)/3` 计算压力。每个机制保存 rxx、rzz、rxz、ryy 四个量。曲面自由边界的牵引变化率闭合显式包含记忆项，不能只在完整步后把应力投影为零。

时间步同时受未松弛高速极限、所选吸收边界、震源采样和 `dt <= 0.25/max(omega_l)` 约束；这是一项保守实现限制，不是普适最优稳定上限。二维可选[海绵或 CFS-PML（ADE）](CFS_PML_2D.md)，PML 另检查辅助极点上限，其速度梯度修正同时用于 GSLS 驱动。外边界数值吸收与材料 Q 本构耗散分别设置；两后端的组合状态对比及 5 s 长尾已检查。

## 在界面中使用

二维“建模 → Qp / Qs 黏弹性（GSLS）”启用。均匀模型填写 Qp/Qs；分层模型在原层表新增的 Qp、Qs 两列逐层设置。指定拟合频带、参考频率和机制数，应用 / 重建后生效。当前参数面板显示实际机制数、参考频率、松弛时间步和最大频带拟合误差。一个机制不能保证宽频带恒 Q，应检查拟合误差和源的有效频带。

模型 VTS 默认海绵使用二维 schema 2，保存 `GSLSSettings` 和逐层 `LayerQpQs`；选择 PML 或修改其参数则用 schema 3，另保存吸收配置。启用外扩吸收带使用 schema 4，另记录外扩设置；底带延用目标底部 Q 与物性，目标域不施加边界阻尼。schema 1 仍能读取为无衰减模型。波场 VTS 包含衰减设置元数据，材料导出包含 Qp/Qs；压力或正应力导出附带真实 Syy。CSV 标明衰减、频带、参考频率及 Q。模型载入从零时刻开始，不是记忆变量检查点续算。

## 内存与性能

无 Q 时五个状态场保持原实现。启用后是 `6+4L` 个状态场：L=1/2/3 时分别 10/14/18 个。当前七套 RK 缓冲一起积分记忆变量；这是经过验证的首版，尚未采用低存储 RK 或指数分裂优化。预算同时计入系数和状态；在原 CUDA 预算口径下，L=3 的节点成本约为无 Q 的三倍，实际还受记录及临时资源影响。

正常显示只传输所选物理分量；普通波场快照不下载全部记忆变量。`snapshot(true)` 才显式取回记忆状态供验证。诊断中的能量使用未松弛弹性等效形式，是有限性/异常增长代理量，**不是黏弹性总储能，也不是耗散能量积分**。

## 已完成的复现与验证

`seis2d_viscoelastic_validation` / CTest `curvilinear_2d_gsls_attenuation`：

- Qp/Qs=40/25、Vp/Vs=2000/1000 m/s、rho=2000 kg/m³、三机制、3..30 Hz、参考 10 Hz：161 个对数频率点的最大相对 Q 误差 3.3113%，参考相速度相对误差低于 1e-13。
- 200 m 周期 P/S 模态，32/64/128 节点，独立求解 `s²+k² C(s)/rho=0` 得解析衰减与相位。各自推进两周期，细网格复振幅误差最大 0.025381%，完整速度矢量误差低于 0.1%。不是用同一 FD 程序生成“参考解”。

解析模态从与该模态一致的非零波场/记忆初值开始，检验内点传播；Ricker 点源、分层和自由表面另由受控模拟检查，两者的验证对象不同。
- 当前 DRP 小数系数的零阶矩为 -0.00006，一阶矩为 0.99998；本测试存在约 2.5e-4 的误差底，误差不严格随加密单调下降。本轮未改原传播系数，不据此宣称新的收敛阶。
- 1/2/3 机制参考速度、强/弱衰减参数、大 Q 极限、非被动 Q 对拒绝、曲面原始牵引变化率闭合、CPU/CUDA 波场/Syy/记忆变量/0.5 ms 记录一致性（逐数组相对容差 1e-8）、模型往返及清空通过。
- Q 界面实际勾选、键盘编辑、应用、推进和逐层表格通过。关闭 Q 后，既有二维算例五场及三物性数组与修改前完全相同。既有二维物理、模型 IO、震源和后台发布回归通过。

原始结果在外层 `out/validation/viscoelastic`。独立解析测试验证了内点本构与传播，曲面闭合检查验证边界代数；这些不等同于已完成黏弹性 Rayleigh 半空间解析记录、复杂层界面或工区精度验证。

## 受控黄土示例与重绘

```powershell
# 从内层仓库运行；需 numpy、matplotlib，先完成 Release 构建。
python -B tools/reproduce_qp_qs.py --run --backend cuda
```

没有 CUDA 时显式改用 `--backend cpu`；只重绘已有数据时去掉 `--run`。输出统一位于外层 `out/research/qp-qs-2d`，包含 REPORT.md、index.html、PNG/SVG、manifest.json，以及各工况模型、原始记录、波场和描述性波窗评价。

六工况均为 481×241、h=1 m、15 Hz、0.6 s：平地无 Q、平地 Qp/Qs=40/25、起伏无 Q、起伏 Qp/Qs=40/25、20/12.5、80/50。低速层厚 40 m、Vp/Vs=900/400 m/s，下层 2000/1000 m/s；地层按计算深度对齐，起伏振幅 8 m。它是自定义实验，不冒用论文原始参数。同一地形内的 Q 对照保持源、接收和参考速度不变；平地/起伏组使用相同计算索引，实际物理坐标随映射变化并记录在文件中。统一记录时间轴，绘图使用共同幅值，不做 AGC。不同稳定 dt 的半程快照标注各自实际时间。

目前示例展示可解释的衰减及频散趋势；尚未在固定物理源宽度下完成整套网格加密、海绵/边界距离敏感性和生产频带验收，不能直接作为施工最优参数。

## 靖边5 km四组地形—低速层对照

在原15 Hz、2 m的M00/M10/M01/M11四组实验上，每组分别关闭/启用Q，构成8个工况。
运行入口为 `tools/run_jingbian_q_comparison.py --run`，分析与重绘入口为
`tools/analyze_jingbian_q_comparison.py`；Python需要NumPy、SciPy与Matplotlib。
先构建Release目标 `seis2d_experiment` 和 `seis_gsls_fit_curves`，不带`--run`时仅分析已有完整结果。

本轮截取原剖面X=2.5–7.5 km，保留完整高程序列并平移横坐标，避免改变截取边界的插值斜率。
计算范围为5 km×1.5 km、2501×751节点、2 m网格、1 s；保留原15 Hz爆炸矩源、
源计算深度30 m、40 m道距、0.5 ms采样。126道中以中央50道作为主分析区域。
它保留1000 m第一目的界面，不包含2000 m第二目的界面；应先与原10 km无Q记录的重叠区核对。

低降速带三层的假设Qp/Qs依次为20/12.5、40/25、80/50，背景层为150/90和200/120。
三机制，参考频率15 Hz、拟合频带3–45 Hz。这些是未标定试验值，不是工区实测Q。
开启Q后自动使用其稳定步长，双方记录在共同采样轴比较。

产物输出到外层 `out/research/jingbian-q-5km`。`SIMULATIONS_COMPLETE.txt`只标记8组计算完成；
`ANALYSIS_COMPLETE.txt`才表示原实验核对、统计及图件也已生成。
各工况的`model/model.vts`可导入当前二维界面，包含完整建模与Q设置；记录保留Float64 CSV及SEG-Y，
图件同时提供PNG/SVG。分析明确区分原始振幅与仅供显示的AGC，接收速度平方积分不作为总机械能或信噪比。

批量工具`seis2d_experiment`新增`--width-m`、`--depth-m`、`--duration-s`、`--origin-x-m`、`--q on|off`，
默认仍为原10 km×4 km、3 s、关闭Q。5 km局部三机制CUDA预算约2.08 GiB；
原5001×2001节点三机制预算约11.0 GiB，不能据本轮局部运行声称原完整模型已在8 GB设备上验证。

## Q 是否更接近真实资料

若实际介质存在显著吸收，加入经过标定的 Q 能表示无耗散弹性模型缺少的材料耗散与相伴频散，因而**有可能**改善波形和频谱吻合。但 Q 还可能误补偿速度错误、地形散射、数值耗散、源子波或炮检耦合误差；二维与三维几何扩散也不同。波形变弱、变平滑本身不证明模型更真实。

下一步应以实测微测井/VSP/井间资料约束 Q，匹配源子波、仪器响应、分量和采样；分别检查到时、相位、绝对/相对振幅、频谱及目标波窗，在未参与标定的炮点或道上检验。当前没有导入现场实测记录进行拟合，所以本轮不报告“与实测吻合提高了多少”。
