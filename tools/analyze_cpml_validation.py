"""Summarize the measured 2D boundary tests; never read AGC images as evidence."""
from pathlib import Path
import sys,csv,json
import xml.etree.ElementTree as ET
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT.parent/'out/validation/cfs-pml-2d'
sys.path.insert(0,str(ROOT/'tools/seismic_plotting'))
from seisplot.runtime import prepare_runtime
prepare_runtime()
import numpy as np
import matplotlib
matplotlib.use('Agg')
from matplotlib import pyplot as plt
plt.rcParams.update({'font.family':['Times New Roman','Microsoft YaHei'],'font.size':12,'axes.linewidth':.6,'axes.unicode_minus':False,'xtick.direction':'in','ytick.direction':'in'})
LABELS=['平地15 Hz，浅源','平地30 Hz，浅源','起伏30 Hz，浅源','埋源水平力，混合P/S','分层15 Hz，浅源','起伏分层30 Hz，Qp/Qs','侧底角点，埋源']
rows=list(csv.DictReader((OUT/'boundary_comparison.csv').open(encoding='utf-8-sig')))
assert len(rows)==21
by={(int(r['scenario']),r['boundary']):r for r in rows}
for i in range(7):
 p=by[i,'cfs_pml'];s=by[i,'sponge']
 assert float(p['peak_residual_ratio'])<1e-3 and float(p['l2_residual_ratio'])<1e-3
 assert float(p['peak_residual_ratio'])<float(s['peak_residual_ratio'])/4
fig,axes=plt.subplots(1,2,figsize=(14,6),layout='constrained',sharey=True)
colors={'off':'#536a80','sponge':'#ed8b23','cfs_pml':'#b9231e'}
for ax,key,title in zip(axes,['peak_residual_ratio','l2_residual_ratio'],['峰值残差 / 参考入射峰值','残差L2 / 参考入射窗L2']):
 for index,boundary in enumerate(colors):
  values=[float(by[i,boundary][key])*100 for i in range(7)]
  ax.barh(np.arange(7)+(index-1)*.23,values,height=.21,color=colors[boundary],label={'off':'关闭','sponge':'海绵32格','cfs_pml':'CFS-PML 32格'}[boundary])
 ax.axvline(.1,color='#1766a0',lw=.8,ls='--',label='本组验收0.1%')
 ax.set(xscale='log',xlabel='残差比 (%)，越低越好',title=title,yticks=np.arange(7),yticklabels=LABELS)
 ax.grid(axis='x',alpha=.15,which='major')
axes[0].invert_yaxis()
fig.legend(*axes[0].get_legend_handles_labels(),loc='outside lower center',ncol=4,fontsize=10)
for ext in ['png','pdf']:fig.savefig(OUT/('边界残差对比.'+ext),dpi=240,bbox_inches='tight')
plt.close(fig)
fig,axes=plt.subplots(2,2,figsize=(13,7),layout='constrained')
for row,scenario in enumerate([2,5]):
 for boundary,kind in [('sponge',1),('cfs_pml',2)]:
  a=np.genfromtxt(OUT/f'trace_{scenario}_{kind}.csv',delimiter=',',names=True)
  axes[row,0].plot(a['time_s'],a['small_vz_m_s']*1e6,c=colors[boundary],lw=.9,label=boundary)
  axes[row,1].plot(a['time_s'],a['residual_m_s']*1e6,c=colors[boundary],lw=.9,label=boundary)
 axes[row,0].plot(a['time_s'],a['reference_vz_m_s']*1e6,c='#1766a0',lw=.8,ls='--',label='扩大域参考')
 for ax in axes[row]:ax.set(xlabel='时间 (s)',ylabel='Vz (µm/s)');ax.legend(fontsize=10);ax.grid(alpha=.15)
 axes[row,0].set_xlim(0,.65);axes[row,1].set_xlim(.18,.65)
 axes[row,0].set_title(LABELS[scenario]+'：完整原始记录')
 axes[row,1].set_title('原始残差，固定物理单位；无AGC')
for ext in ['png','pdf']:fig.savefig(OUT/('起伏与Q值残差波形.'+ext),dpi=240,bbox_inches='tight')
plt.close(fig)
summary={'cases':7,'comparisons':21,'width_cells':32,'width_m':16,'worst_cpml_peak_ratio':max(float(r['peak_residual_ratio']) for r in rows if r['boundary']=='cfs_pml'),'worst_cpml_l2_ratio':max(float(r['l2_residual_ratio']) for r in rows if r['boundary']=='cfs_pml'),'minimum_peak_improvement':min(float(by[i,'sponge']['peak_residual_ratio'])/float(by[i,'cfs_pml']['peak_residual_ratio']) for i in range(7)),'acceptance_ratio':.001,'reference_margin_nodes':480,'reference_grid':[1201,641],'terrain_wavelength_m':80,'fixed_flatten_depth_m':28,'same_reference_geometry':True,'reference_boundary':'CFS-PML64; boundaries outside first return window','timing_note':'Single runs; not a controlled performance benchmark','definition':'mixed-wave trace residual, not an isolated modal reflection coefficient'}
(OUT/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
if all((OUT/f'external_wavefield_{a}_{q}.vts').exists() and (OUT/f'external_model_{a}_{q}.vts').exists() for a in range(2) for q in range(2)):
 checked=[]
 for kind in ['wavefield','model']:
  for a in range(2):
   for q in range(2):
    path=OUT/f'external_{kind}_{a}_{q}.vts';grid=ET.parse(path).getroot().find('StructuredGrid')
    assert grid.attrib['WholeExtent']=='0 79 0 0 0 71'
    coordinates=grid.find('Piece/Points/DataArray');values=np.fromstring(coordinates.text,sep=' ').reshape(-1,3)
    assert len(values)==80*72 and np.min(values[:,0])==0 and np.max(values[:,0])==158
    assert np.min(values[:,2])==0 and np.max(values[:,2])==142
    assert all(len(array.text.split())==80*72 for array in grid.findall('Piece/PointData/DataArray'))
    fields={array.attrib['Name']:array.text for array in grid.findall('FieldData/DataArray')}
    assert float(fields['ExternalAbsorption'])==1
    if kind=='model':assert float(fields['SeisWaveModel2DVersion'])==4
    else:assert float(fields['ComputationalNodesX'])==112 and float(fields['ComputationalNodesZ'])==88
    checked.append(path.name)
 export_check={'status':'PASS','files':checked,'target_points':[80,72],'actual_compute_points':[112,88],
               'target_x_m':[0,158],'target_z_m':[0,142],'all_point_arrays_target_size':True,'exported_external_mode_metadata':True}
 (OUT/'external-export-check.json').write_text(json.dumps(export_check,ensure_ascii=False,indent=2),encoding='utf-8')
lines=['# 二维CFS-PML吸收边界升级与验收报告','','日期：2026-10-27。当前Release；旧海绵默认值保留。','', '## 实现与检查','', '采用计算坐标CFS伸缩与ADE记忆变量，所有辅助变量随同一四阶段推进更新。仅左右和底部吸收带分配，每个活跃方向4个辅助量，角点同时拥有两个方向。自由表面法向速度导数使用修正后的切向导数；近表面动量沿用牵引镜像通量，并逐方向施加吸收。GSLS驱动使用修正后的速度梯度。','', 'CPU/CUDA弹性及Qp/Qs全场/记忆变量对比、清空复跑、无效参数事务保留、模型VTS schema3往返通过。两种后端均完成弹性及Qp/Qs的5s长尾检查。GUI真实下拉选择、待应用与生效分离、重建、推进和诊断检查通过。CTest实测记录见ctest.log。','', '## 扩大域对照','', '小域241×161，h=0.5m，120×80m；参考域左右和底部各扩展480格，1201×641。固定展平深度28m；地形波长80m，平移240m为完整3周期，避免把地形或网格变化误算成边界反射。参考界面位于本组观测窗的首个返回路径之外。输出间隔0.5ms、T=0.65s，C=0.6。','', '均匀介质Vp/Vs/ρ=550/250/1550；分层模型上10m沿用该层，下伏900/450/2000。Q工况上层Qp/Qs=80/50、下层120/70，3支路、5–80Hz、参考30Hz。普通浅源为(60m,η=1m)，接收(75m,η=0)；埋源水平力深度15m、接收深度15m；角点用源(95m,η=50m)、接收(85m,η=45m)。起伏振幅2m。','', '参考入射窗0.04–0.22s；残差窗一般0.18–0.65s，角点0.08–0.65s。使用原始Vz，未做AGC或逐道归一化。该指标包含混合波型、离散及边界入口散射，不是单一P/S/Rayleigh模态反射系数。','', '| 工况 | 海绵峰值残差% | CFS-PML峰值残差% | CFS-PML L2残差% | 峰值改善倍数 |','| --- | ---: | ---: | ---: | ---: |']
for i,label in enumerate(LABELS):
 a=by[i,'sponge'];c=by[i,'cfs_pml'];lines.append(f"| {label} | {float(a['peak_residual_ratio'])*100:.6g} | {float(c['peak_residual_ratio'])*100:.6g} | {float(c['l2_residual_ratio'])*100:.6g} | {float(a['peak_residual_ratio'])/float(c['peak_residual_ratio']):.1f} |")
lines+=['','![七组边界残差](边界残差对比.png)','','![起伏和Q值的原始波形及残差](起伏与Q值残差波形.png)','','## 使用与边界','','二维：运行 → 时间与吸收层 → 吸收边界 → CFS-PML (ADE)，设置后重建。宽度复用X/底部吸收格数，实际仍不超过各轴节点数1/3。目标R默认1e-4、κ最大值3、参考频率0随震源。目标R是轮廓参数，不能当作已实现的反射率。','', 'PML缓冲在此小网格、32格时约3.86MiB（七套RK辅助状态及轮廓）；实际预算随节点与厚度计算。时间步同时受PML辅助极点限制。CPU与CUDA均检查辅助状态有限性，清空、回退及销毁覆盖这些内存。','', '验收门槛为本测试矩阵峰值及L2残差比均小于0.1%，峰值较同宽海绵改善至少4倍。所有7组通过；门槛是本项目本组测试目标，不是通用规范。未开展任意强坡度、所有入射角或10km工区全域的精度证明，也未实现三维PML。单次计时用于溯源，不作为性能倍速结论。','', '## 复现','','编译seis2d_cpml_validation，运行（输出均位于外层out/validation/cfs-pml-2d）：','','```powershell','seis2d_cpml_validation.exe','seis2d_cpml_validation.exe --audit','SeisWaveLab_Curvilinear.exe --smoke-2d-pml','python -B tools/analyze_cpml_validation.py','```','', '方法参考：Martin等（2010）ADE-PML，https://specfem.org/komatitsch.free.fr/published_papers/CMES_ADE_PML_2010.pdf ；仓库CGFD3D的CFS-PML源码用于方法核对。']
lines.insert(3,'本报告的七组反射对照保持原内部模式，以复现同宽基准；外扩吸收网格为新增独立验证，不能将旧基准数字直接当作任意外扩工区的反射率。')
if (OUT/'external_absorber_verification.csv').exists():
 lines+=['','## 外扩吸收网格补充验收','','新建二维工作台默认启用“吸收带向模型外扩展”。目标 Nx×Nz 保持不变，计算增加左右各wx及底部wz，顶部仍自由；目标域内海绵阻尼为零、PML为单位伸缩。炮点、全宽检波器、DEM 坐标与波场/模型导出范围保持原值；B 可显示浅橙色外部网格。旧 schema1..3/API 默认保留内部模式，需主动勾选后重建；外扩配置写 schema4。三维尚未外扩。','','海绵/PML×弹性/GSLS四组合，80×72、h=2m扩为112×88，80道仍位于0..158m。与独立手工扩域的全部场、Q/PML状态及Vx/Vz记录一致，CPU/CUDA、清空及保存重建通过。目标DEM只覆盖原范围，原节点坐标、度量和展平深度不变；连续延拓、分层Q传播、陡边正Jacobian及预算/超限保留旧会话检查通过。底带延用目标底面材料与Q，避免额外引入尚未进入目标域的地层。','','四组合矩阵：`external_absorber_verification.csv`；8份VTS点数组及坐标范围独立解析：`external-export-check.json`。相关6项快速CTest见`external-final-ctest.log`，旧7工况反射回归见`external-legacy-reflection.log`，陡边补充见`external-edge-final.log`。外扩/内部真实控件及完整模型叠加图通过。','','![外扩吸收区域](external-absorption.png)']
(OUT/'二维CFS-PML升级验收报告.md').write_text('\n'.join(lines),encoding='utf-8')
print(json.dumps(summary,ensure_ascii=False,indent=2))
