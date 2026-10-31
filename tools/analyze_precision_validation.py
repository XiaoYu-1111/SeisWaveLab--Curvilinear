"""Summarize measured 2D precision evidence. Default output: outer out/ only."""
from pathlib import Path
import argparse
import csv
import json
import re
import shutil
import statistics
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / 'out/validation/precision-2d'
sys.path.insert(0, str(ROOT / 'tools/seismic_plotting'))
from seisplot.runtime import prepare_runtime
prepare_runtime()
import numpy as np
import matplotlib
matplotlib.use('Agg')
from matplotlib import pyplot as plt

plt.rcParams.update({'font.family': ['Times New Roman', 'Microsoft YaHei'], 'font.size': 11,
                     'axes.linewidth': .65, 'axes.unicode_minus': False,
                     'xtick.direction': 'in', 'ytick.direction': 'in'})
COLORS = ['#a82a23', '#1966a7']

def rows(name):
    with (OUT / name).open(encoding='utf-8-sig') as f:
        return list(csv.DictReader(f))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--publish', action='store_true', help='Publish reviewed documentation assets to docs/')
    args = parser.parse_args()
    assert (OUT / 'PASS.txt').is_file(), 'Run numerical validation first'
    from PIL import Image
    Image.open(OUT/'ui/fp32-parameters.ppm').save(OUT/'ui/fp32-parameters.png')
    test_log = (ROOT.parent/'out/build/precision-final-tests.log').read_text(encoding='utf-8-sig')
    tests = re.search(r'100% tests passed, 0 tests failed out of (\d+)', test_log)
    assert tests, 'Complete final CTest verification first'
    tests_passed = int(tests.group(1))
    accuracy, storage, bench, capacity, tail = [rows(n) for n in ['accuracy.csv', 'storage.csv', 'benchmark.csv', 'capacity.csv', 'long_tail.csv']]
    assert len(bench) == 36 and len(capacity) == 4
    assert len(accuracy) > 300 and max(float(r['relative_l2']) for r in accuracy) < .001
    host = json.loads((OUT / 'host.json').read_text(encoding='utf-8-sig'))
    groups = {}
    for row in bench:
        assert row['status'] == 'OK'
        key = row['case'], int(row['nx']), row['backend'], row['precision']
        groups.setdefault(key, []).append(row)
    med = {k: statistics.median(float(r['step_ms']) for r in v) for k, v in groups.items()}
    summary = {'date': '2026-10-28', 'default_precision': 'FP64', 'related_ctests_passed': tests_passed, 'host': host,
               'max_relative_l2': max(float(r['relative_l2']) for r in accuracy),
               'raw_records_max_relative_l2': max(float(r['relative_l2']) for r in accuracy if r['component'] == 'raw_records'),
               'late_records_max_relative_l2': max(float(r['relative_l2']) for r in accuracy if r['component'] == 'late_raw_records'),
               'cpu32_cuda32_max_relative_l2': max(float(r['relative_l2']) for r in accuracy if r['pair'] == 'CUDA32_CPU32'),
               'groups': [], 'capacity': capacity}
    perf_table = ['| 后端 / 逻辑网格 | 方程 | FP64 ms/步 | FP32 ms/步 | FP32倍速 | FP64 / FP32设备数组 GiB |',
                  '| --- | --- | ---: | ---: | ---: | ---: |']
    for backend, size in [('CUDA', 1024), ('CUDA', 2048), ('CPU8', 512)]:
        for case, label in [('elastic_PML', '弹性+PML'), ('GSLS3_PML', '3支路GSLS+PML')]:
            a, b = med[case, size, backend, 'FP64'], med[case, size, backend, 'FP32']
            v = [groups[case, size, backend, p][0] for p in ['FP64', 'FP32']]
            memory = [int(r['device_payload_bytes']) / 2**30 for r in v]
            spread = [100 * (max(float(r['step_ms']) for r in groups[case, size, backend, p]) - min(float(r['step_ms']) for r in groups[case, size, backend, p])) / med[case, size, backend, p] for p in ['FP64', 'FP32']]
            summary['groups'].append(dict(backend=backend, nx=size, case=case, fp64_ms=a, fp32_ms=b, speedup=a/b, device_gib=memory, range_percent=spread))
            perf_table.append(f'| {backend} / {size}² | {label} | {a:.2f} | {b:.2f} | {a/b:.2f}× | '+ (' / '.join(f'{x:.3f}' for x in memory) if backend == 'CUDA' else '—（主机数组减半）') + ' |')
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.8), layout='constrained')
    labels = ['1024²\nElastic+PML', '1024²\nGSLS3+PML', '2048²\nElastic+PML', '2048²\nGSLS3+PML']
    gpu = [r for r in summary['groups'] if r['backend'] == 'CUDA']
    x = np.arange(4)
    for i, precision in enumerate(['FP64', 'FP32']):
        values = [r[precision.lower()+'_ms'] for r in gpu]
        axes[0].bar(x+(i-.5)*.34, values, width=.34, color=COLORS[i], label=precision)
        axes[1].bar(x+(i-.5)*.34, [r['device_gib'][i] for r in gpu], width=.34, color=COLORS[i], label=precision)
    for ax, ylabel in zip(axes[:2], ['Median time / step (ms)', 'Device allocation payload (GiB)']):
        ax.set(xticks=x, xticklabels=labels, ylabel=ylabel); ax.grid(axis='y', alpha=.16);ax.legend()
    for case, label, color in [('elastic_PML', 'Elastic + PML', COLORS[0]), ('GSLS3_PML', 'GSLS3 + PML', COLORS[1])]:
        g = [r for r in gpu if r['case'] == case]
        axes[2].plot([r['nx']**2/1e6 for r in g], [r['speedup'] for r in g], 'o-', lw=1, color=color, label=label)
    axes[2].axhline(1, color='.5', lw=.6, ls='--');axes[2].set(xlabel='Logical grid points (million)', ylabel='FP64 time / FP32 time', title='CUDA precision speedup');axes[2].legend();axes[2].grid(alpha=.16)
    for ext in ['png', 'pdf']:fig.savefig(OUT / ('precision_performance.'+ext), dpi=220, bbox_inches='tight')
    plt.close(fig)
    data = np.genfromtxt(OUT / 'dry_loess_traces.csv', delimiter=',', names=True)
    fig, axes = plt.subplots(1, 2, figsize=(12, 4), layout='constrained')
    axes[0].plot(data['time_s'], data['cuda64_vz']*1e6, lw=1, c=COLORS[0], label='CUDA FP64')
    axes[0].plot(data['time_s'], data['cuda32_vz']*1e6, lw=.8, c=COLORS[1], ls='--', label='CUDA FP32')
    axes[1].plot(data['time_s'], (data['cuda32_vz']-data['cuda64_vz'])*1e6, lw=.75, c=COLORS[1])
    for ax in axes:ax.set(xlabel='Time (s)', ylabel='Vz (µm/s)');ax.grid(alpha=.16)
    axes[0].set_title('Dry loess + GSLS3 + CFS-PML; raw trace');axes[0].legend();axes[1].set_title('FP32 − FP64; no AGC / normalization')
    for ext in ['png', 'pdf']:fig.savefig(OUT / ('precision_raw_trace.'+ext), dpi=220, bbox_inches='tight')
    plt.close(fig)
    fig, axes = plt.subplots(1, 2, figsize=(12, 4), layout='constrained')
    for component, color in [('Vz', COLORS[1]), ('Szz', COLORS[0])]:
        g = [r for r in tail if r['component'] == component]; peak = max(float(r['reference_peak']) for r in g)
        axes[0].semilogy([float(r['time_s']) for r in g], [float(r['reference_peak'])/peak for r in g], lw=.9, c=color, label=component+' FP64 signal / main peak')
        axes[0].semilogy([float(r['time_s']) for r in g], [max(float(r['error_peak'])/peak, 1e-18) for r in g], lw=.8, c=color, ls='--', label=component+' error / main peak')
        axes[1].semilogy([float(r['time_s']) for r in g], [max(float(r['relative_l2']), 1e-18) for r in g], lw=.9, c=color, label=component)
    for ax in axes:ax.set(xlabel='Time (s)');ax.grid(alpha=.16);ax.legend(fontsize=9)
    axes[0].set(ylabel='Normalized peak amplitude', title='Long tail: absolute error vs main signal')
    axes[1].set(ylabel='L2 error / current FP64 L2', title='Relative error grows as tail vanishes')
    for ext in ['png', 'pdf']:fig.savefig(OUT / ('precision_long_tail.'+ext), dpi=220, bbox_inches='tight')
    plt.close(fig)
    tail_final = {r['component']: r for r in tail if float(r['time_s']) == 5}
    summary['tail_final'] = tail_final
    summary['tail_max_error_over_main_peak'] = {c: max(float(r['error_peak']) for r in tail if r['component'] == c) / max(float(r['reference_peak']) for r in tail if r['component'] == c) for c in tail_final}
    (OUT / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    main = f'''# 二维FP32/FP64实现、精度与资源实测

2026-10-29更新：当前CPU32线程与GPU性能模式、四档模型规模及两种精度的正式性能比较见[最新二维性能报告](LATEST_PERF_LINK)。下文保留2026-10-28的精度资格、CPU8性能和5001×2001容量证据，不能与新测试混合计算倍速；默认仍为FP64。

2026-10-28，Windows Release；CPU：{host['cpu'].strip()}，固定8线程性能对照；GPU：RTX 4060 Laptop，8188 MiB，驱动621.01。主机内存{host['ram_bytes']/2**30:.2f} GiB。**默认仍为FP64**，本轮不自动改变历史模型精度。只实现二维，三维仍为FP64。

最终Release相关CTest {tests_passed}/{tests_passed}通过：弹性/后端、GSLS、模型IO、后台发布、PML状态、外扩吸收、七组反射审计、PML界面、精度数值及精度界面。性能计时另以固定步数运行。

## 实现与操作

二维“运行 → 计算核心 → 计算精度”选择FP32或FP64，然后应用/重建。切换清空当前波场和记录，从t=0开始；待应用值与已生效值分开显示。模型数组没有必须永久使用FP64的理由，本轮让坐标、法向、度量、Vp/Vs/密度/Q、弹性模量、源权重、GSLS系数、波场、RK暂存及PML记忆全部随选定精度存储和计算，不保留完整双精度镜像。

几何构建、GSLS拟合、时间轴及能量/牵引诊断保留FP64；完成构建后逐数组转换并释放临时double数组，再分配所选精度的工作区。少量参数表、原始高程剖面、接收坐标和CSV记录仍以double保存，不是逐网格模型镜像。FP32采样值转换为double不会恢复丢失的有效位。预算同时考虑构建瞬时FP64占用，溢出、失去正值的量化物性/几何拒绝初始化。

模型保存FP32时采用二维schema 5：点数组和坐标为Float32，`CalculationPrecision=1`参加完整性校验。FP64继续按原schema 2/3/4写出；旧schema 1..4读入FP64。载入仍从零重建。波场VTS保留所选精度，CSV带计算/模型精度注释，显示色带和增益不进入原始导出。没有开启fast-math或FP16。

![红黄橙主题下的FP32参数与实际波场](ui/fp32-parameters.png)

## 小模型逐级验证

80×72逻辑网格；外扩左右/底部12格，另有3层虚点。CPU单线程及CUDA，依次比较平地弹性、曲面爆炸矩源、曲面PML、1支路GSLS+PML、3支路GSLS+PML、分层干黄土+3支路GSLS+PML。普通组h=1 m、20 Hz、0.35 s；干黄土组h=0.25 m、15 Hz、0.4 s，上层厚3 m、Vp/Vs=500/150 m/s、ρ=1450、Qp/Qs=40/25，下层1200/600 m/s、ρ=1900、Qp/Qs=80/50。干黄土源深索引5，起伏0.15 m、波长20 m，记录间隔0.25 ms。普通材料550/250 m/s、ρ=1550；Q拟合5–60 Hz、参考20 Hz。此处小尺寸用于舍入误差资格验证，不是靖边工区地质复现。

比较全部五场、独立Syy、各GSLS记忆、紧凑PML状态、原始Vx/Vz及前后半窗记录。预设门槛：相对L2≤0.1%、相对峰值≤0.3%；牵引相对误差FP32≤5×10⁻⁶、FP64≤10⁻⁹。未使用AGC或逐道归一化。

结果：全部比较中最大L2为{summary['max_relative_l2']*100:.5f}%；原始接收记录最大L2为{summary['raw_records_max_relative_l2']*100:.6f}%，后半窗最大{summary['late_records_max_relative_l2']*100:.6f}%。本机构建下CPU32/CUDA32最大L2为{summary['cpu32_cuda32_max_relative_l2']:.3g}，不承诺其他编译器逐位一致。主机模型、RK状态和边界数组分别恰好减半；GPU还保留固定大小的显示/FP64归约和接收缓冲，总量略大于一半。清空复跑、无效重建保留旧状态、schema 5校验及模型往返、后台发布与双向界面切换均已验收。

![干黄土原始记录和误差](precision_raw_trace.png)

### 极弱长尾的限制

48×40、GSLS3+PML、5 s、每约0.1 s检查一次，共51个快照。全时段误差峰值低于主信号峰值的0.01%，无不稳定能量增长；但不能据此宣称消失尾波的相对精度合格。5 s末FP64/FP32能量相对各自峰值为{float(tail_final['Vz']['reference_energy_ratio']):.3g}/{float(tail_final['Vz']['fp32_energy_ratio']):.3g}。Vz相对L2约{float(tail_final['Vz']['relative_l2'])*100:.2f}%，Szz约{float(tail_final['Szz']['relative_l2'])*100:.0f}%；Szz绝对误差峰值仅{float(tail_final['Szz']['error_peak']):.3g} Pa，Vz为{float(tail_final['Vz']['error_peak']):.3g} m/s。**强衰减后极弱尾波、微小残余应力或差分信号研究仍优先FP64**；能量代理也不是GSLS全部记忆的严格储能。

![长尾绝对和相对误差分开查看](precision_long_tail.png)

## 中、大网格速度与显存

性能模型保持相同物理设置与dt，仅切换精度：h=2 m，15 Hz，Vp/Vs=1500/700 m/s、ρ=1800，起伏5 m、波长500 m，CFL=0.5；外扩16格CFS-PML，32检波器，0.25 ms输出。Q组3支路、Qp/Qs=40/25、5–60 Hz、参考20 Hz。名义时长10 s、步数上限1000，**本测试仅推进指定步数，未完成10 s正演**。这些物性不代表500/150 m/s干黄土在2 m网格上的空间精度。

每配置预热20步；CUDA计时60步、CPU计时20步，各交错重复3次，取中位数。计时包含4阶段RK、表面投影、PML/GSLS更新、每步诊断和接收回传；排除初始化、OpenGL显示、导出及全场下载。每次仅保留一个求解器。`device_payload_bytes`直接累加实际cudaMalloc请求，包括设备模型/RK/PML、显示、统计和接收数组；不是整进程显存或CUDA上下文占用。另记录预热前后的可用显存，受WDDM和其他应用影响；主机数组统计不含所有容器、记录及驱动开销。

'''+ '\n'.join(perf_table) + '''

![中大网格时间、设备数组与倍速](precision_performance.png)

FP32对GPU的收益已实测，不能以理论浮点峰值比直接预测整程序。CPU弹性没有加速，GSLS仅小幅改善；本实现每步仍有双精度能量归约、内存遍历、边界与同步，主机循环也未另做SIMD优化。CUDA RHS元数据报告FP64/FP32分别163/140寄存器、localSizeBytes 368/240字节每线程；这不是完整的Nsight瓶颈分析，也不能把local字节全部解释为寄存器溢出。

## 5001×2001容量实测

逻辑10,007,001点；外扩16格和虚点后实际5039×2023＝10,193,897点。各配置预热8步、计时20步，仅一次容量试运行；不与重复3次的中大网格统计混同。

| 配置 | 实际设备数组 | ms/步 | 结果 |
| --- | ---: | ---: | --- |
'''
    for row in capacity:
        ok = row['status'] == 'OK'
        main += f"| {row['case']} / {row['precision']} | {int(row['device_payload_bytes'])/2**30:.3f} GiB | {float(row['step_ms']):.2f} | 运行通过 |\n" if ok else f"| {row['case']} / {row['precision']} | — | — | 显存预检拒绝，未运行 |\n"
    target = next(r for r in capacity if r['case'] == 'GSLS3_PML' and r['precision'] == 'FP32')
    main += f'''
GSLS3/PML的FP32实际设备数组{int(target['device_payload_bytes'])/2**30:.5f} GiB，GPU模式主机模型数组约{int(target['host_array_bytes'])/2**30:.3f} GiB；预热后可用显存约{int(target['free_after_bytes'])/2**30:.2f} GiB。对应FP64保守预算11.04 GiB，预留后需空闲12.99 GiB，本机当时仅6.94 GiB，故拒绝；没有这个组合的实测FP64耗时和直接倍速。两种精度的弹性容量测试均运行，但属于单次短测试。

均匀体力源、3机制Q的主要逐点存储：CPU模型22+七套状态126＝148个所选精度标量，FP64/FP32为1184/592字节；GPU静态16+状态126＝142标量，1136/568字节，另加紧凑PML、显示与统计缓冲；GPU主机保留22个模型标量。无Q时CPU49、GPU45标量；爆炸矩源另加2个梯度标量。PML七套记忆用`7×4×(2wxNz+wzNx)`个所选精度标量，Nx/Nz为含吸收带、不含虚点的计算尺寸。以上不是总进程容量公式。

## 默认精度决定与后续验收

保留FP64默认。FP32可手动用于已验证范围的常规二维弹性/GSLS和显存受限试算，尤其本机5001×2001的Q模型。大模型容量通过不等于完整3–10 s工区正演、所有低Q/陡地形或弱反射振幅分析都已合格。调整默认前，应使用目标5/10 km DEM、实际干黄土速度及相应更细网格，完整对照主频、相位、弱反射、面波频散和采集评价窗口；采用未归一化数据与网格收敛结果。FP32不会解决h过大导致的数值频散。

本机中大GPU实测支持单精度节省资源；极弱长尾和CPU实测又说明全局强制替换FP64不合适。三维精度、低存储RK、稀疏诊断/异步回传和混合精度优化留待单独验证。切换大模型仍需为候选会话留出空间；重建失败保留旧结果，必要时先导出并返回主页释放旧会话。

## 复现与原始证据

在VS x64开发环境构建外层`out/build/x64-Release`的`seis2d_precision_validation`及主程序，然后运行：

```powershell
../out/bin/Release/seis2d_precision_validation.exe
../out/bin/Release/SeisWaveLab_Curvilinear.exe --smoke-2d-precision
../out/bin/Release/seis2d_precision_validation.exe --benchmark
../out/bin/Release/seis2d_precision_validation.exe --capacity
ctest --test-dir ../out/build/x64-Release -R "curvilinear_2d_|curvilinear_background_publication" --output-on-failure > ../out/build/precision-final-tests.log
python -B tools/analyze_precision_validation.py
```

运行测试前关闭其他模拟会话。默认数据输出外层`out/validation/precision-2d`；脚本不自动更改默认精度。`--publish`仅发布当前验收的说明资源。

证据：[逐分量误差](accuracy.csv)、[存储和牵引](storage.csv)、[重复性能测试](benchmark.csv)、[大模型容量](capacity.csv)、[51时刻长尾](long_tail.csv)、[摘要](summary.json)。[测试源码](SOURCE_VALIDATION)、[分析脚本](SOURCE_ANALYSIS)。
'''
    (OUT / '二维精度验证与性能实测.md').write_text(main.replace('LATEST_PERF_LINK','../../../SeisWaveLab_Curvilinear/docs/PERFORMANCE_PRECISION_2D.md').replace('SOURCE_VALIDATION','../../../SeisWaveLab_Curvilinear/src/two_d/PrecisionValidation2D.cpp').replace('SOURCE_ANALYSIS','../../../SeisWaveLab_Curvilinear/tools/analyze_precision_validation.py'), encoding='utf-8')
    if args.publish:
        dest = ROOT/'docs/data/precision-2d-2026-10-28';images = ROOT/'docs/images/precision-2d'
        dest.mkdir(parents=True, exist_ok=True);images.mkdir(parents=True, exist_ok=True)
        for name in ['accuracy.csv','storage.csv','benchmark.csv','capacity.csv','long_tail.csv','dry_loess_traces.csv','summary.json','host.json','gpu.csv','PASS.txt']:
            shutil.copy2(OUT/name, dest/name)
        for name in ['precision_performance.png','precision_raw_trace.png','precision_long_tail.png']:
            shutil.copy2(OUT/name, images/name)
        shutil.copy2(OUT/'ui/fp32-parameters.png', images/'fp32-parameters.png')
        published = main.replace('ui/fp32-parameters.png','images/precision-2d/fp32-parameters.png')
        for name in ['precision_performance.png','precision_raw_trace.png','precision_long_tail.png']:
            published = published.replace(']('+name+')','](images/precision-2d/'+name+')')
        for name in ['accuracy.csv','storage.csv','benchmark.csv','capacity.csv','long_tail.csv','summary.json']:
            published = published.replace(']('+name+')','](data/precision-2d-2026-10-28/'+name+')')
        published = published.replace('LATEST_PERF_LINK','PERFORMANCE_PRECISION_2D.md').replace('SOURCE_VALIDATION','../src/two_d/PrecisionValidation2D.cpp').replace('SOURCE_ANALYSIS','../tools/analyze_precision_validation.py')
        (ROOT/'docs/PRECISION_2D.md').write_text(published, encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k in ['max_relative_l2','raw_records_max_relative_l2','late_records_max_relative_l2','groups']}, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    main()
