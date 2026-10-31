"""Build the measured CPU32/GPU precision report; --publish updates documentation assets."""
from pathlib import Path
import argparse
import csv
from collections import defaultdict
from datetime import datetime, timezone, timedelta
import gzip
import hashlib
import json
import shutil
import statistics as S
import sys
import xml.etree.ElementTree as ET
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent/'out'
sys.path.insert(0, str(ROOT/'tools/seismic_plotting'))
from seisplot.runtime import prepare_runtime
prepare_runtime()
import numpy as np
import matplotlib
matplotlib.use('Agg')
from matplotlib import pyplot as plt

MODES = [('CPU','FP64'),('CPU','FP32'),('GPU','FP64'),('GPU','FP32')]
CASES = ['elastic_PML','GSLS3_PML']
COLORS = ['#a92720','#e58a24','#193b70','#2a9ac2']
plt.rcParams.update({'font.family':['Times New Roman','Microsoft YaHei'],'font.size':11,
                    'axes.linewidth':.65,'axes.unicode_minus':False,'xtick.direction':'in','ytick.direction':'in'})

def write_csv(path, rows):
    with path.open('w',newline='',encoding='utf-8-sig') as f:
        keys = list(dict.fromkeys(k for row in rows for k in row))
        w = csv.DictWriter(f,fieldnames=keys);w.writeheader();w.writerows(rows)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',nargs='?',type=Path,default=OUT/'benchmarks/precision_cpu32_gpu_20261029')
    p.add_argument('--publish',action='store_true')
    a = p.parse_args();folder = a.directory.resolve()
    assert folder.is_relative_to(OUT.resolve()) and (folder/'COMPLETE.txt').is_file()
    manifest = json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
    assert manifest['status'] == 'complete' and not manifest['telemetry_errors']
    trials = json.loads((folder/'trials.json').read_text(encoding='utf-8'))
    assert len(trials) == len(manifest['sizes'])*2*4*manifest['repeats']
    for r in trials:r['case'] = {'elastic':'elastic_PML','gsls3':'GSLS3_PML'}.get(r['case'],r['case'])
    t = np.genfromtxt(folder/'telemetry.csv',delimiter=',',names=True,dtype=float)
    groups = defaultdict(list)
    for r in trials:
        if r['status'] != 'OK':continue
        assert r['actual_omp_team'] == 32 and r['ac_power'] == 1 and r['mxcsr'] & 0x8040 == 0
        assert r['process_affinity'] == r['system_affinity'] and r['completed_step'] == r['warmup_steps']+r['timed_steps']
        if r['backend'] == 'CPU':assert r['solver_workers'] == 32
        sample = t[(t['epoch']>=r['timed_start_epoch']) & (t['epoch']<=r['timed_end_epoch'])]
        r['telemetry_samples'] = len(sample)
        for field in ['power_w','enforced_limit_w','gpu_util_percent','sm_mhz','memory_mhz','temperature_c','pstate']:
            values = sample[field];values = values[np.isfinite(values)]
            for stat,fn in [('mean',np.mean),('min',np.min),('max',np.max)]:
                r[field+'_'+stat] = float(fn(values)) if len(values) else None
        r['samples_sha256'] = hashlib.sha256((folder/'trials'/r['run_id']/'samples.csv').read_bytes()).hexdigest()
        groups[r['size'],r['case'],r['backend'],r['precision']].append(r)
    write_csv(folder/'trials.csv',trials)
    summary = []
    for size in manifest['sizes']:
        for case in CASES:
            for backend,precision in MODES:
                rr = groups[size,case,backend,precision]
                reject = [r for r in trials if (r['size'],r['case'],r['backend'],r['precision']) == (size,case,backend,precision) and r['status']!='OK']
                g = dict(size=size,logical_nodes=size*size,case=case,backend=backend,precision=precision,n=len(rr),rejected=len(reject))
                if rr:
                    assert len({r['samples_sha256'] for r in rr}) == 1, 'Repeat state differs'
                    assert len({r['dt_s'] for r in rr}) == 1
                    ms = [r['step_ms'] for r in rr]
                    g.update(median_ms=S.median(ms),p25_ms=float(np.quantile(ms,.25)),p75_ms=float(np.quantile(ms,.75)),min_ms=min(ms),max_ms=max(ms),
                             range_percent=100*(max(ms)-min(ms))/S.median(ms),steps_per_s=1000/S.median(ms),logical_mnodes_per_s=size*size/(1000*S.median(ms)))
                    for field in ['host_array_bytes','device_payload_bytes','working_set_bytes','peak_working_set_bytes_before_snapshot','cpu_busy_cores','cpu_util_of_32_percent','initialization_ms','compute_nodes']:
                        g[field] = S.median(r[field] for r in rr)
                    timed = np.concatenate([t[(t['epoch']>=r['timed_start_epoch']) & (t['epoch']<=r['timed_end_epoch'])] for r in rr])
                    g['telemetry_samples'] = len(timed)
                    for field in ['power_w','enforced_limit_w','gpu_util_percent','sm_mhz','temperature_c']:
                        values = timed[field];values = values[np.isfinite(values)]
                        if len(values):g.update({field+'_mean':float(np.mean(values)),field+'_min':float(np.min(values)),field+'_max':float(np.max(values))})
                    g['p0_percent'] = float(100*np.mean(timed['pstate']==0)) if len(timed) else None
                summary.append(g)
    by = {(r['size'],r['case'],r['backend'],r['precision']):r for r in summary}
    def speed(size,case,b1,p1,b2,p2):
        g1,g2 = by[size,case,b1,p1],by[size,case,b2,p2]
        return g1['median_ms']/g2['median_ms'] if g1['n']>=3 and g2['n']>=3 else None
    for g in summary:
        g['gpu_speedup_same_precision'] = speed(g['size'],g['case'],'CPU',g['precision'],'GPU',g['precision'])
        g['fp32_speedup_same_backend'] = speed(g['size'],g['case'],g['backend'],'FP64',g['backend'],'FP32')
    write_csv(folder/'summary.csv',summary)
    checks = []
    for size in manifest['sizes']:
        for case in CASES:
            good = [m for m in MODES if groups[size,case,*m]]
            refmode = ('CPU','FP64') if ('CPU','FP64') in good else ('GPU','FP64')
            assert refmode in good
            def read_samples(mode):
                r = groups[size,case,*mode][0]
                v = np.genfromtxt(folder/'trials'/r['run_id']/'samples.csv',delimiter=',',names=True)
                return r,v
            ref,rv = read_samples(refmode)
            velocity_peak = max(np.max(np.abs(rv['value'][rv['component']==i])) for i in [0,1])
            assert velocity_peak > 1e-25, 'Zero comparison wavefield'
            for mode in good:
                r,v = read_samples(mode);assert r['dt_s'] == ref['dt_s']
                assert np.array_equal(rv['component'],v['component']) and np.array_equal(rv['index'],v['index']) and np.all(np.isfinite(v['value']))
                for section,mask in [('sampled_fields',rv['component']<6),('all_receivers',rv['component']>=6)]:
                    worst_l2=worst_peak=max_abs=0.
                    for component in np.unique(rv['component'][mask]):
                        x=rv['value'][rv['component']==component];y=v['value'][v['component']==component];d=y-x
                        err=np.linalg.norm(d)/max(np.linalg.norm(x),1e-150);pe=np.max(np.abs(d))/max(np.max(np.abs(x)),1e-150)
                        worst_l2=max(worst_l2,float(err));worst_peak=max(worst_peak,float(pe));max_abs=max(max_abs,float(np.max(np.abs(d))))
                    threshold=1e-8 if mode[1]=='FP64' else 1e-3
                    weak=bool(section=='all_receivers' and max_abs<=1e-5*velocity_peak)
                    passed=bool(worst_l2<=threshold and worst_peak<=3*threshold or weak)
                    checks.append(dict(size=size,case=case,backend=mode[0],precision=mode[1],reference='_'.join(refmode),section=section,
                                       relative_l2=worst_l2,relative_peak=worst_peak,max_abs=max_abs,velocity_peak=velocity_peak,weak_window=weak,pass_check=passed))
    write_csv(folder/'consistency.csv',checks)
    assert all(r['pass_check'] for r in checks), 'Comparison outside predeclared tolerance; inspect consistency.csv'
    def save(fig,name):
        for ext in ['png','pdf']:fig.savefig(folder/f'{name}.{ext}',dpi=220,bbox_inches='tight')
        plt.close(fig)
    sizes = manifest['sizes'];x = np.array(sizes)**2/1e6
    for kind,name,ylabel in [('median_ms','step_time','Wall time per accepted step (ms)'),('logical_mnodes_per_s','throughput','Logical grid throughput (Mpoint-step/s)')]:
        fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout='constrained')
        for ax,case in zip(axes,CASES):
            for i,(backend,precision) in enumerate(MODES):
                gs=[by[n,case,backend,precision] for n in sizes]
                y=[g.get(kind,np.nan) if g['n']>=3 else np.nan for g in gs]
                ax.plot(x,y,marker='o' if precision=='FP64' else 's',ls='-' if precision=='FP64' else '--',lw=1.1,color=COLORS[i],label=f'{backend}{" 32T" if backend=="CPU" else ""} {precision}')
                if kind=='median_ms':
                    low=[g.get('p25_ms',np.nan) if g['n']>=3 else np.nan for g in gs];high=[g.get('p75_ms',np.nan) if g['n']>=3 else np.nan for g in gs]
                    ax.fill_between(x,low,high,color=COLORS[i],alpha=.12)
            ax.set(xlabel='Logical grid points (million)',ylabel=ylabel,title=case.replace('_',' + '));ax.set_yscale('log');ax.grid(alpha=.15);ax.legend(fontsize=9)
            if by[sizes[-1],case,'CPU','FP64']['n']==0:ax.text(.98,.02,'CPU 32T FP64: RAM guard at largest grid',transform=ax.transAxes,ha='right',fontsize=8,color=COLORS[0])
        save(fig,name)
    fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout='constrained')
    for ax,case in zip(axes,CASES):
        for i,p in enumerate(['FP64','FP32']):
            ax.plot(x,[speed(n,case,'CPU',p,'GPU',p) or np.nan for n in sizes],marker='os'[i],lw=1.1,color=COLORS[i*2],label=p)
        ax.axhline(1,c='.5',lw=.6,ls='--');ax.set(xlabel='Logical grid points (million)',ylabel='CPU (32T) time / GPU time',title=case.replace('_',' + '));ax.grid(alpha=.15);ax.legend()
    save(fig,'gpu_speedup')
    fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout='constrained')
    for ax,case in zip(axes,CASES):
        for backend,color in [('CPU',COLORS[0]),('GPU',COLORS[2])]:
            ax.plot(x,[speed(n,case,backend,'FP64',backend,'FP32') or np.nan for n in sizes],'o-',lw=1.1,c=color,label='CPU 32T' if backend=='CPU' else backend)
        ax.axhline(1,c='.5',lw=.6,ls='--');ax.set(xlabel='Logical grid points (million)',ylabel='FP64 time / FP32 time',title=case.replace('_',' + '));ax.grid(alpha=.15);ax.legend()
    save(fig,'precision_speedup')
    fig,axes=plt.subplots(1,2,figsize=(12,4.6),layout='constrained')
    for ax,backend,field in [(axes[0],'CPU','host_array_bytes'),(axes[1],'GPU','device_payload_bytes')]:
        for i,(case,precision) in enumerate([(c,p) for c in CASES for p in ['FP64','FP32']]):
            gs=[by[n,case,backend,precision] for n in sizes]
            ax.plot(x,[g.get(field,np.nan)/2**30 for g in gs],marker='os'[i%2],ls='-' if precision=='FP64' else '--',lw=1,c=COLORS[i],label=case+' '+precision)
        ax.set(xlabel='Logical grid points (million)',ylabel='Owned array payload (GiB)',title='CPU32 host arrays' if backend=='CPU' else 'GPU device arrays');ax.grid(alpha=.15);ax.legend(fontsize=8)
    save(fig,'memory')
    fig,axes=plt.subplots(3,1,figsize=(12,7),layout='constrained',sharex=True)
    minutes=(t['epoch']-manifest['started_epoch'])/60
    axes[0].plot(minutes,t['power_w'],lw=.6,c=COLORS[0],label='Reported GPU power');axes[0].plot(minutes,t['enforced_limit_w'],lw=.8,c=COLORS[2],ls='--',label='Enforced limit');axes[0].set_ylabel('Power (W)');axes[0].legend(ncol=2)
    axes[1].plot(minutes,t['sm_mhz'],lw=.6,c=COLORS[2]);axes[1].set_ylabel('SM clock (MHz)')
    axes[2].plot(minutes,t['temperature_c'],lw=.6,c=COLORS[1]);axes[2].set(xlabel='Elapsed test session (min)',ylabel='GPU temperature (°C)')
    for ax in axes:ax.grid(alpha=.15)
    save(fig,'gpu_telemetry')
    # Markdown narrative and publication use the same measured aggregates.
    generate_report(folder,manifest,trials,summary,checks,by,speed,a.publish)

def generate_report(folder,manifest,trials,summary,checks,by,speed,publish):
    ok=[r for r in trials if r['status']=='OK'];rejected=[r for r in trials if r['status']!='OK']
    pc=manifest['power_control'];gpu=ET.fromstring(pc['before_gpu']['stdout'])
    first=ok[0];tz=timezone(timedelta(hours=8))
    started=datetime.fromtimestamp(manifest['started_epoch'],tz).strftime('%Y-%m-%d %H:%M:%S')
    finished=datetime.fromtimestamp(manifest['finished_epoch'],tz).strftime('%Y-%m-%d %H:%M:%S')
    def f(x,n=2):return '—' if x is None else f'{x:.{n}f}'
    def timecell(size,case,backend,precision):
        g=by[size,case,backend,precision]
        return f'{g["median_ms"]:.3f}' + (f'（{g["n"]}/{manifest["repeats"]}）' if g['n']<manifest['repeats'] else '') if g['n'] else '资源预检拒绝'
    chosen=[g for g in summary if g['backend']=='GPU' and g['n']>=3]
    cpu=[g for g in summary if g['backend']=='CPU' and g['n']>=3]
    aggregate={
        'gpu_power_w_min':min(g.get('power_w_min',float('inf')) for g in chosen),
        'gpu_power_w_max':max(g.get('power_w_max',0) for g in chosen),
        'gpu_enforced_limit_min_w':min(g.get('enforced_limit_w_min',float('inf')) for g in chosen),
        'gpu_enforced_limit_max_w':max(g.get('enforced_limit_w_max',0) for g in chosen),
        'gpu_temperature_max_c':max(g.get('temperature_c_max',0) for g in chosen),
        'gpu_sm_min_mhz':min(g.get('sm_mhz_min',float('inf')) for g in chosen),
        'gpu_sm_max_mhz':max(g.get('sm_mhz_max',0) for g in chosen),
        'cpu_busy_cores_median_min':min(g['cpu_busy_cores'] for g in cpu),
        'cpu_busy_cores_median_max':max(g['cpu_busy_cores'] for g in cpu),
        'max_sampled_field_l2':max(r['relative_l2'] for r in checks if r['section']=='sampled_fields'),
        'max_receiver_l2':max(r['relative_l2'] for r in checks if r['section']=='all_receivers'),
        'receiver_max_absolute_error':max(r['max_abs'] for r in checks if r['section']=='all_receivers'),
        'weak_receiver_max_absolute_error':max((r['max_abs'] for r in checks if r['section']=='all_receivers' and r['relative_l2']>1e-3),default=0),
        'weak_receiver_checks':sum(r['weak_window'] and (r['relative_l2']>1e-3 or r['relative_peak']>3e-3) for r in checks),
    }
    result={'date':'2026-10-29','attempted':len(trials),'success':len(ok),'resource_rejected':len(rejected),
            'cpu_threads':32,'gpu_mode':'User-confirmed vendor performance mode; no software-enforced 140W',
            'gpu':gpu.findtext('gpu/product_name'),'driver':gpu.findtext('kmd_version') or gpu.findtext('driver_version'),
            'ram_bytes':first['ram_bytes'],'cpu':first['cpu'],'groups':summary,'conditions':aggregate,
            'all_sample_checks_pass':all(r['pass_check'] for r in checks),
            'executable_sha256':manifest['executable_sha256'],'source_sha256':manifest['benchmark_source_sha256']}
    result['gpu_max_power_request_verified'] = pc['verified_limits']['enforced_w'] == pc['before_limits']['max_w']
    result['windows_high_performance_request_succeeded'] = pc['set_high_performance']['returncode'] == 0
    (folder/'summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    public_manifest={k:v for k,v in manifest.items() if k not in ['power_control','pid']}
    public_manifest['power_control']={k:v for k,v in pc.items() if k!='before_gpu'}
    confirmation=folder/'environment-confirmation.json'
    if confirmation.is_file():public_manifest['vendor_mode_user_confirmation']=json.loads(confirmation.read_text(encoding='utf-8'))
    # Keep identifiers/full NVIDIA diagnostic XML in local audit only.
    (folder/'manifest-public.json').write_text(json.dumps(public_manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    text=f'''# 二维CPU 32线程／GPU：FP32与FP64规模性能实测

测试日期：2026-10-29。实测区间：{started} 至 {finished}（北京时间）。本报告更新CPU32线程与GPU的性能证据；不改软件默认FP64，不修改求解器数值内核。原有精度与弱尾波限制仍见[二维精度验证](PRECISION_LINK)。

## 测试条件与“最大功率”的实际含义

- CPU：{first['cpu'].strip()}，32个逻辑处理器。所有成功试验实际OpenMP团队均为32，CPU求解器报告32工作线程，进程可使用全部逻辑处理器；进程优先级Above Normal，`OMP_DYNAMIC=FALSE`、`OMP_WAIT_POLICY=PASSIVE`，CPU保留非正规数，不启用FTZ/DAZ。
- GPU：{result['gpu']}，驱动{result['driver']}；主机内存{first['ram_bytes']/2**30:.2f} GiB。交流电状态由每个测试进程核验，用户确认已开启厂商性能／GPU高性能模式。
- 已尝试`nvidia-smi -pl 140`，驱动返回“不支持当前作用域修改功率”。本次GPU计时窗读到的实际强制上限为{aggregate['gpu_enforced_limit_min_w']:.0f}–{aggregate['gpu_enforced_limit_max_w']:.0f} W；**不能将本结果称为已锁定140 W的满功率测试**。上限不是实际耗电，实际报告功耗约{aggregate['gpu_power_w_min']:.1f}–{aggregate['gpu_power_w_max']:.1f} W，负载决定能否接近上限。
- Windows拒绝切换`SCHEME_MIN`，电源计划GUID保持原`381b4222-f694-41f0-9685-ff5bb260df2e`（平衡）；用户的厂商性能模式与Windows计划分开记录。测试后执行恢复原计划，命令和实测功率限制见[环境清单](manifest-public.json)。没有启用超频，也没有并发其他求解器来人为拉高功耗。
- 100 ms一次NVML遥测覆盖整轮测试，计时窗单独匹配；功耗属于驱动报告值，部分读数本身有平均窗口，短试验不据此作高精度能耗积分。[NVIDIA官方说明](https://docs.nvidia.com/deploy/nvidia-smi/index.html)区分功率上限、当前强制上限和实际功耗。

本次共计划{len(trials)}次：{len(ok)}次成功、{len(rejected)}次资源预检拒绝。每配置5次，不剔除离群值；正式比值要求至少3次成功。界面、模型初始化、清空、显式场下载和导出不进入计时。

## 模型矩阵与计时口径

四档逻辑网格256²、1024²、1536²、2048²；每档包含弹性+CFS-PML与三支路GSLS+CFS-PML。h=2 m，Vp/Vs=1500/700 m/s，ρ=1800 kg/m³；起伏5 m、波长500 m，15 Hz定向体力源，CFL=0.5；32检波器、输出间隔0.25 ms。Q组Qp/Qs=40/25、5–60 Hz拟合、参考20 Hz；两组均匀介质，便于区分增加GSLS状态的开销，不代表已标定黄土地层。

左右各外扩16格、底部外扩16格；另有每面3层虚点。逻辑N²对应无虚点计算域(N+32)×(N+16)，存储尺寸(N+38)×(N+22)。逻辑跨度为(N−1)×2 m。所有CPU/GPU、FP32/FP64组合采用同一输入和该工况的同一dt。FP32模型和传播/边界数组均为单精度，时间轴和能量归约保留双精度。

每次独立启动进程，先用相同生产求解器预热至少1 s，然后清空并推进固定预热步数；正式计时步数如下。每轮轮换CPU/GPU及精度顺序以减少固定顺序偏差。每个相同规模的四组合接受相同数量时间步。

| 逻辑网格 | 存储节点数（含外扩与虚点） | 清空后预热步 | 正式计时步／重复 |
| --- | ---: | ---: | ---: |
'''
    for n in manifest['sizes']:
        warm,steps=manifest['warmup_timed_steps'][str(n)]
        text+=f'| {n}² | {(n+38)*(n+22):,} | {warm} | {steps} × {manifest["repeats"]} |\n'
    text+='''
单步耗时为生产`advance`的实际墙钟耗时/有效步数，包含四阶段RK、表面投影、GSLS/PML、每步诊断/回退保护及接收回传和插值。没有关闭同步来制造GPU倍速。图中带状区域是5次结果的25%–75%分位，不是置信区间。横轴吞吐以逻辑网格点数计算，初始化、工作集及设备数组容量另列。

名义时长10 s只是提供稳定的dt规划；试验实际止于上述步数，**没有完成每个模型的10 s物理正演**。这是计算效率测试，不把有限场值、短时一致性或粗网格容量当作工区波形精度保证。

## 计算效率结果

各列单位ms/有效步，取成功试验中位数。GPU加速比为同精度CPU32耗时/GPU耗时；无匹配数据的位置不计算比值。

'''
    for case,title in zip(CASES,['弹性＋CFS-PML','三支路GSLS＋CFS-PML']):
        text+=f'### {title}\n\n| 规模 | CPU32 FP64 | CPU32 FP32 | GPU FP64 | GPU FP32 | GPU加速FP64 | GPU加速FP32 |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n'
        for n in manifest['sizes']:
            cells=[timecell(n,case,*mode) for mode in MODES]
            ratios=[f(speed(n,case,'CPU',p,'GPU',p))+'×' if speed(n,case,'CPU',p,'GPU',p) is not None else '—' for p in ['FP64','FP32']]
            text+=f'| {n}² | '+' | '.join(cells+ratios)+' |\n'
        text+='\n'
    text+='''![CPU32与GPU单步耗时，阴影为四分位范围](step_time.png)

![同精度GPU相对CPU32的倍速](gpu_speedup.png)

![逻辑网格点吞吐](throughput.png)

### 单精度收益单独比较

下表为同后端FP64耗时/FP32耗时，大于1才表示FP32更快；不能把GPU/CPU倍速与精度倍速混为一谈。

| 规模 | 方程 | CPU32中FP32倍速 | GPU中FP32倍速 |
| --- | --- | ---: | ---: |
'''
    for n in manifest['sizes']:
        for case in CASES:
            vals=[speed(n,case,b,'FP64',b,'FP32') for b in ['CPU','GPU']]
            text+=f'| {n}² | {case} | '+' | '.join(f(v)+'×' if v is not None else '—' for v in vals)+' |\n'
    text+='''
![FP32相对FP64的独立倍速曲线](precision_speedup.png)

CPU32小网格可能受并行调度、阶段同步和串行诊断限制，减半数组不保证更快；大网格还受缓存、带宽和内存压力影响。GPU的FP32收益也包含访存与寄存器变化，不能直接以芯片浮点峰值比代替程序实测。此处未做Nsight分析，不把这些可能因素当作已经定位的唯一瓶颈。

## 内存、显存与资源超限

表中为求解器实际拥有数组的容量，单位GiB；CPU列是CPU求解模式的主机模型/RK/PML数组，GPU列是实际设备分配请求总和，含显示、统计及接收缓冲。它们不是同一进程的主机/设备两部分。GPU模式另保留选定精度的主机模型，完整进程工作集、初始化峰值、WDDM可用显存见原始CSV。

| 规模 | 方程 | CPU FP64数组 | CPU FP32数组 | GPU FP64数组 | GPU FP32数组 |
| --- | --- | ---: | ---: | ---: | ---: |
'''
    for n in manifest['sizes']:
        for case in CASES:
            vals=[]
            for b,p in MODES:
                g=by[n,case,b,p];field='host_array_bytes' if b=='CPU' else 'device_payload_bytes'
                vals.append(f(g[field]/2**30,3) if g['n'] else '预检拒绝')
            text+=f'| {n}² | {case} | '+' | '.join(vals)+' |\n'
    text+='\n![主机与设备数组容量](memory.png)\n\n'
    if rejected:
        reasons=sorted(set(r['reason'] for r in rejected))
        text+='本轮超限记录如下，均未绕过资源保护，也未把拒绝写为零耗时或无限加速：\n\n'
        for reason in reasons:text+='- '+reason.replace('\n',' ')+'\n'
    else:text+='本轮所有配置均通过资源预检。此结论只适用于测试时空闲内存与显存条件。\n'
    text+=f'''
## 满线程、利用率与GPU性能状态

实际OpenMP团队均为32，并不意味着程序每个阶段都保持100% CPU利用率。按计时窗进程CPU秒数/墙钟秒数计算，CPU试验组的平均忙核数中位数约{aggregate['cpu_busy_cores_median_min']:.1f}–{aggregate['cpu_busy_cores_median_max']:.1f}；相对于32个逻辑处理器的利用率和完整原始值保留在CSV。没有额外运行占位线程伪造满载。CPU包功率和有效频率未直接测量，不宣称已锁定全部核心最高睿频。

GPU计时窗遥测最高温度{aggregate['gpu_temperature_max_c']:.0f} °C，SM频率范围{aggregate['gpu_sm_min_mhz']:.0f}–{aggregate['gpu_sm_max_mhz']:.0f} MHz。下表为同配置所有计时窗样本的均值及峰值；100 ms遥测可能让极短窗口只有少数样本，不能据此把“当前没跑到功率上限”解释为模式没有开启。原始时序另有P状态、显存利用率及限频原因原始mask；没有仅凭功耗推定瓶颈。

| 规模 | 方程 | 精度 | GPU平均/峰值W | 平均利用率% | 平均SM MHz | 最高温度°C | 遥测样本 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
'''
    for g in chosen:
        text+=f'| {g["size"]}² | {g["case"]} | {g["precision"]} | {f(g.get("power_w_mean"),1)} / {f(g.get("power_w_max"),1)} | {f(g.get("gpu_util_percent_mean"),1)} | {f(g.get("sm_mhz_mean"),0)} | {f(g.get("temperature_c_max"),0)} | {g["telemetry_samples"]} |\n'
    text+='''
![整轮GPU功率、强制上限、频率和温度；低谷包含CPU测试阶段](gpu_telemetry.png)

## 一致性与结论边界

同一后端、精度和工况内，5次的采样场和全部检波记录哈希一致。跨后端/精度另作误差比较：额外下载在计时结束后进行，五个物理场（Q组另有Syy）取1024个均匀存储索引及源周围25×25节点，另比较全部32位置的Vx/Vz样本。它是性能试验的一致性检查，不是完整GSLS/PML记忆状态的逐点验证。

'''
    text+=f'采样场最大相对L2为{aggregate["max_sampled_field_l2"]:.5g}；按FP64同精度1e-8、涉及FP32为1e-3的L2门槛检查，峰值门槛为3倍。每道原始记录也单独比较；短记录可能在主波到达前，允许绝对误差小于源区速度峰值1e-5的极弱窗另行标识，本轮此分支使用{aggregate["weak_receiver_checks"]}项。记录中最大逐道相对L2达到{aggregate["max_receiver_l2"]*100:.1f}%，这部分没有通过统一的相对误差门槛；这些弱窗的整体峰值绝对误差不超过{aggregate["weak_receiver_max_absolute_error"]:.3g} m/s。远偏移主波尚未到达，部分极小数值在FP32下归零，因此不能用本次短窗宣称远偏移反射振幅相对精度。详见[一致性原始表](consistency.csv)。\n\n'
    text+='''新结果用于选择当前机器上的计算后端与精度。默认仍保留FP64，二维FP32可手动用于显存受限或常规试算；强衰减后极弱应力尾波的相对误差限制不因速度快而消失。h=2 m的本组1500/700 m/s均匀材料，不能直接替代干黄土500/150 m/s的网格精度论证。三维、本轮未完成的长时工区正演、更多设备和厂商模式，均不作推广保证。

与2026-10-28的CPU8线程结果、2026-10-19的二维/三维FP64旧基准分开保留；这些测试的线程数、模型、接收数、PML和驱动条件不同，不拼接成同一倍速曲线。当前说明中的二维CPU32/FP32/FP64性能结论以本页为准。

## 复现与文件

在x64 MSVC开发环境，从工作区外层构建：

```powershell
cmake --build out/build/x64-Release --target seis2d_precision_benchmark --parallel 4
```

从仓库根目录执行，接交流电并先开启厂商性能模式；使用新的外层out子目录可保留原始证据：

```powershell
python -B benchmarks/precision_suite.py --output <外层out下新的绝对目录>
python -B benchmarks/report_precision.py <该绝对目录>
```

`--resume`继续已保存试验，`--sizes`可限定规模；不应用于更改硬件模式后拼接数据。环境恢复由runner的finally执行，命令及核验值均记录。`--publish`将验收图文作为文档资源发布，默认分析只写外层out。

原始[逐次试验](trials.csv)、[配置统计](summary.csv)、[环境清单](manifest-public.json)、[数值检查](consistency.csv)、[完整遥测压缩文件](telemetry.csv.gz)、[统计JSON](summary.json)。源程序：[C++基准](CPP_LINK)、[运行和遥测](RUNNER_LINK)、[报告生成](REPORT_LINK)。EXE SHA-256：`'''+manifest['executable_sha256']+'`。本工作区保留既有未提交改动，未自动提交或推送。\n'
    text=text.replace('5次',f'{manifest["repeats"]}次')
    text=text.replace('没有启用超频','测试脚本没有修改频率或启用超频')
    with (folder/'telemetry.csv').open('rb') as src,gzip.open(folder/'telemetry.csv.gz','wb') as dst:shutil.copyfileobj(src,dst)
    local=text.replace('PRECISION_LINK','../../../SeisWaveLab_Curvilinear/docs/PRECISION_2D.md').replace('CPP_LINK','../../../SeisWaveLab_Curvilinear/benchmarks/PrecisionBenchmark2D.cpp').replace('RUNNER_LINK','../../../SeisWaveLab_Curvilinear/benchmarks/precision_suite.py').replace('REPORT_LINK','../../../SeisWaveLab_Curvilinear/benchmarks/report_precision.py')
    (folder/'二维CPU32线程_GPU精度性能测试报告.md').write_text(local,encoding='utf-8')
    if publish:
        data=ROOT/'docs/data/precision-cpu32-2026-10-29';images=ROOT/'docs/images/precision-cpu32'
        data.mkdir(parents=True,exist_ok=True);images.mkdir(parents=True,exist_ok=True)
        published=text.replace('PRECISION_LINK','PRECISION_2D.md').replace('CPP_LINK','../benchmarks/PrecisionBenchmark2D.cpp').replace('RUNNER_LINK','../benchmarks/precision_suite.py').replace('REPORT_LINK','../benchmarks/report_precision.py')
        for name in ['trials.csv','summary.csv','consistency.csv','manifest-public.json','summary.json','telemetry.csv.gz']:
            shutil.copy2(folder/name,data/name);published=published.replace(']('+name+')','](data/precision-cpu32-2026-10-29/'+name+')')
        for name in ['step_time','throughput','gpu_speedup','precision_speedup','memory','gpu_telemetry']:
            shutil.copy2(folder/f'{name}.png',images/f'{name}.png');published=published.replace(f']({name}.png)',f'](images/precision-cpu32/{name}.png)')
        (ROOT/'docs/PERFORMANCE_PRECISION_2D.md').write_text(published,encoding='utf-8')
    print(json.dumps({'attempted':len(trials),'success':len(ok),'rejected':len(rejected),'conditions':aggregate},ensure_ascii=False,indent=2))

if __name__=='__main__':
    main()
