"""Run and plot the 2D GSLS method reproduction and controlled synthetic cases.

Requires numpy/matplotlib. Generated products are restricted to the outer out/.
This reproduces a constitutive method and analytical benchmarks, not every figure
of a published field study. No observed data are supplied or fitted.
"""
from pathlib import Path
import argparse
import csv
import hashlib
import json
import subprocess
import shutil
import zipfile
from datetime import datetime, timezone
import xml.etree.ElementTree as ET
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / 'out'


def save(fig, directory, name):
    for extension in ('png', 'svg'):
        fig.savefig(directory / f'{name}.{extension}', dpi=220, bbox_inches='tight')
    plt.close(fig)


def records(path):
    data = np.genfromtxt(path, delimiter=',', comments='#', skip_header=0,
                         names=['receiver', 'x', 'z', 'time', 'vx', 'vz'], invalid_raise=False)
    # The non-comment CSV header is filtered explicitly; no amplitude processing.
    data = data[np.isfinite(data['receiver'])]
    ids = np.unique(data['receiver']).astype(int)
    traces = [data[data['receiver'] == i] for i in ids]
    time = traces[0]['time']
    assert all(np.array_equal(t['time'], time) for t in traces)
    return time, np.array([t['x'][0] for t in traces]), np.stack([t['vz'] for t in traces])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--archive-source', action='store_true', help='Archive the current source worktree and complete result folder; requires git.')
    parser.add_argument('--backend', choices=['cpu', 'cuda'], default='cuda')
    parser.add_argument('--output', type=Path, default=OUT / 'research/qp-qs-2d')
    args = parser.parse_args()
    folder = args.output.resolve()
    if not folder.is_relative_to(OUT.resolve()):
        parser.error('All generated products must remain under the outer out directory.')
    folder.mkdir(parents=True, exist_ok=True)
    validation = OUT / 'validation/viscoelastic'
    executable = OUT / 'bin/Release/seis2d_q_experiment.exe'
    if args.run:
        producer={'started_utc':datetime.now(timezone.utc).isoformat(),'requested_backend':args.backend,'executable_sha256':hashlib.sha256(executable.read_bytes()).hexdigest(),'scope':'synthetic controlled experiment'}
        for command, log in [([OUT / 'bin/Release/seis2d_viscoelastic_validation.exe'], 'validation.log'),
                             ([executable, folder, args.backend], 'experiments.log')]:
            with (folder / log).open('w', encoding='utf-8') as stream:
                subprocess.run([str(s) for s in command], check=True, stdout=stream, stderr=subprocess.STDOUT)
        producer['completed_utc']=datetime.now(timezone.utc).isoformat()
        (folder/'producer.json').write_text(json.dumps(producer,indent=2),encoding='utf-8')
    cases = list(csv.DictReader((folder / 'cases.csv').open(encoding='utf-8')))
    assert len(cases) == 6, 'Six completed controlled cases are required.'
    result = json.loads((validation / 'validation.json').read_text(encoding='utf-8'))
    plt.rcParams.update({'font.family': 'serif', 'font.serif': ['Times New Roman', 'DejaVu Serif'],
                         'font.size': 10, 'axes.linewidth': .65, 'lines.linewidth': .85,
                         'xtick.direction': 'in', 'ytick.direction': 'in', 'svg.fonttype': 'none'})
    q = np.genfromtxt(validation / 'q-dispersion.csv', delimiter=',', names=True)
    fig, axes = plt.subplots(2, 2, figsize=(10, 7), constrained_layout=True)
    ax = axes[0, 0]
    for name, label, color in [('fit_qp', 'Qp', '#255b83'), ('fit_qs', 'Qs', '#be5b39')]:
        ax.semilogx(q['frequency_hz'], q[name], color=color, label=label)
    ax.axhline(40, color='#255b83', ls='--', lw=.6); ax.axhline(25, color='#be5b39', ls='--', lw=.6)
    ax.axvspan(3, 30, color='grey', alpha=.09); ax.set(xlabel='Frequency (Hz)', ylabel='Modulus Q', title='(a) Finite-band GSLS fit'); ax.legend()
    ax = axes[0, 1]
    ax.semilogx(q['frequency_hz'], q['vp_phase_m_s'] / 2000, label='Vp / Vp(ref)', color='#255b83')
    ax.semilogx(q['frequency_hz'], q['vs_phase_m_s'] / 1000, label='Vs / Vs(ref)', color='#be5b39')
    ax.axvline(10, color='grey', ls='--', lw=.6); ax.set(xlabel='Frequency (Hz)', ylabel='Relative phase velocity', title='(b) Reference frequency: 10 Hz'); ax.legend()
    for ax, mode in zip(axes[1], ('p', 's')):
        a = np.genfromtxt(validation / f'{mode}-plane-wave.csv', delimiter=',', names=True)
        ax.plot(a['x_m'], a['analytic'], color='black', label='Analytical GSLS')
        ax.plot(a['x_m'], a['numerical'], color='#c34c32', ls='--', label='Curvilinear code: flat interior')
        ax.set(xlabel='X (m)', ylabel='Velocity / initial amplitude', title=f'({"c" if mode == "p" else "d"}) {mode.upper()} mode after two periods'); ax.legend(fontsize=8)
    save(fig, folder, 'gsls_validation')

    names = ['flat_elastic', 'flat_q40', 'curved_elastic', 'curved_q40']
    labels={'flat_elastic':'Flat | elastic','flat_q40':'Flat | Qp/Qs = 40/25','curved_elastic':'Curved | elastic','curved_q40':'Curved | Qp/Qs = 40/25'}
    loaded = {c['case']: records(folder / c['case'] / 'records.csv') for c in cases}
    reference = max(np.max(np.abs(loaded[name][2])) for name in names)
    fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex=True, sharey=True, constrained_layout=True)
    for ax, name in zip(axes.flat, names):
        time, x, values = loaded[name]
        for i in range(0, len(x), 2):
            offset = x[i] - 240
            ax.plot(offset + values[i] / reference * 3.4, time, color='#222222', lw=.45)
        ax.set(title=labels[name], xlabel='Offset X (m)', ylabel='Time (s)', ylim=(.6, 0))
    fig.suptitle('Controlled synthetic records | common amplitude scale | no AGC\n41 of 81 receivers; all 0.5 ms samples', fontsize=11)
    save(fig, folder, 'controlled_records')

    fig, axes = plt.subplots(1, 3, figsize=(13, 3.6), constrained_layout=True)
    colors = {'curved_elastic': '#222222', 'curved_q20': '#bd4835', 'curved_q40': '#28618a', 'curved_q80': '#528446'}
    energy_rows = []
    for name, color in colors.items():
        time, x, values = loaded[name]; dt = float(time[1] - time[0]); trace = values[70]
        legend={'curved_elastic':'Elastic','curved_q20':'Qp/Qs = 20/12.5','curved_q40':'Qp/Qs = 40/25','curved_q80':'Qp/Qs = 80/50'}
        axes[0].plot(time, trace, color=color, label=legend[name])
        window = (time >= .1) & (time < .6)
        amplitude = np.abs(np.fft.rfft(trace[window] * np.hanning(window.sum()))) * dt
        frequency = np.fft.rfftfreq(window.sum(), dt)
        axes[1].plot(frequency, amplitude, color=color)
        late = (time >= .3) & (time < .6)
        energy = np.sum(values[:, late] ** 2, axis=1) * dt
        axes[2].plot(x - 240, energy, color=color)
        energy_rows.append({'case': name, 'all_receiver_integrated_vz2': float(np.sum(values[:, time < .6] ** 2) * dt),
                            'late_window_integrated_vz2': float(energy.sum()), 'peak_vz_m_s': float(np.max(np.abs(values)))})
    axes[0].set(xlabel='Time (s)', ylabel='Vz (m/s)', title='(a) Receiver 71: offset +120 m'); axes[0].legend(fontsize=8)
    axes[1].set(xlabel='Frequency (Hz)', ylabel='Fourier amplitude (m)', title='(b) Same 0.1-0.6 s Hann window', xlim=(0, 80))
    axes[2].set(xlabel='Offset X (m)', ylabel='Integral of Vz squared (m2/s)', title='(c) Descriptive late window: 0.3-0.6 s')
    save(fig, folder, 'q_sensitivity')

    fields = {}
    for name in names:
        document = ET.parse(folder / name / 'wavefield.vts')
        scalar = next(a for a in document.findall('.//PointData/DataArray') if a.attrib['Name'] == 'Vz')
        vz = np.fromstring(scalar.text, sep=' ').reshape(241, 481)
        xyz = np.fromstring(document.find('.//Points/DataArray').text, sep=' ').reshape(241, 481, 3)
        instant = next(float(a.text) for a in document.findall('.//FieldData/DataArray') if a.attrib['Name'] == 'TIME_VALUE')
        fields[name] = (vz, xyz, instant)
    limit = max(np.max(np.abs(a[0])) for a in fields.values())
    fig, axes = plt.subplots(2, 2, figsize=(11, 6.5), sharex=True, sharey=True, constrained_layout=True)
    for ax, name in zip(axes.flat, names):
        vz, xyz, instant = fields[name]
        plot = ax.pcolormesh(xyz[:, :, 0], xyz[:, :, 2], vz, cmap='coolwarm', vmin=-limit, vmax=limit, shading='auto', rasterized=True)
        ax.plot(xyz[0, :, 0], xyz[0, :, 2], color='black', lw=.6)
        ax.set(xlabel='X (m)', ylabel='Depth Z (m)', title=f'{labels[name]} | t={instant:.6f} s', ylim=(240, -12), xlim=(0, 480))
    fig.colorbar(plot, ax=axes, location='bottom', shrink=.75, label='Vz (m/s), shared raw amplitude scale', aspect=45)
    save(fig, folder, 'wavefield_pairs')

    manifest = {'scope': 'GSLS method implementation, independent analytical P/S benchmarks and controlled synthetic cases; not a full paper-figure or field-data reproduction',
                'validation': result, 'cases': cases, 'record_comparison': energy_rows, 'record_common_reference_m_s': float(reference),
                'images': ['gsls_validation', 'controlled_records', 'q_sensitivity', 'wavefield_pairs'], 'observed_data_available': False,
                'units': {'record_integral': 'm2/s; a velocity-squared proxy, not total mechanical energy'}, 'hashes': {}}
    manifest['producer']=json.loads((folder/'producer.json').read_text(encoding='utf-8')) if (folder/'producer.json').is_file() else {'status':'original executable producer hash was not recorded'}
    for path in [executable, OUT / 'bin/Release/seis2d_viscoelastic_validation.exe', Path(__file__).resolve(), ROOT / 'include/SeisCommon/Gsls.hpp', ROOT / 'src/two_d/ViscoelasticValidation2D.cpp', ROOT / 'src/common/Gsls.cpp', ROOT / 'src/two_d/PointKernels2D.hpp', ROOT / 'src/two_d/CurvilinearSolver2D.cpp', ROOT / 'src/two_d/CudaEngine2D.cu'] + list(folder.glob('*/model.vts')) + list(folder.glob('*/records.csv')):
        manifest['hashes'][str(path.relative_to(ROOT.parent))] = hashlib.sha256(path.read_bytes()).hexdigest()
    (folder / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    shutil.copy2(ROOT/'docs/QP_QS_VISCOELASTIC_2D.md',folder/'METHOD.md')
    (folder/'references').mkdir(exist_ok=True)
    shutil.copy2(ROOT/'docs/references/attenuation.bib',folder/'references/attenuation.bib')
    text = ['# 二维 Qp/Qs 方法复现与受控模拟', '',
            '本报告复现 GSLS 本构与记忆变量方法，以独立解析 P/S 模态验证，再运行自定义黄土低速层示例。没有逐图复现特定论文，没有实测记录，不能据此声称已改善工区拟合。', '',
            f"解析细网格复振幅相对误差最大 {result['max_fine_plane_wave_error']:.6%}；3-30 Hz Q 拟合最大相对误差 {result['max_fitted_Q_relative_error']:.3%}。参考相速度、CPU/CUDA、曲面、模型往返及记忆变量清零通过。", '',
            '原 DRP 小数系数的一阶矩为 0.99998，分辨率试验出现约 2.5e-4 的误差底，粗细误差不是严格单调下降；未修改既有传播系数，不据此宣称新的收敛阶。', '',
            '| 工况 | 起伏/m | 顶层 Qp/Qs | dt/ms | 步数 | 核心耗时/s | Q 拟合最大误差 |', '|---|---:|---|---:|---:|---:|---:|']
    for c in cases:
        text.append(f"| {c['case']} | {c['relief_m']} | {c['top_qp']}/{c['top_qs']} ({'启用' if c['attenuation']=='1' else '关闭'}) | {float(c['dt_s'])*1000:.6f} | {c['steps']} | {float(c['compute_s']):.3f} | {float(c['max_q_fit_error']):.2%} |")
    text += ['', '全部为 481×241、h=1 m、15 Hz、0.6 s；上层厚 40 m、Vp/Vs=900/400 m/s、ρ=1800 kg/m³，下层 Vp/Vs=2000/1000 m/s、ρ=2300 kg/m³、Qp/Qs=200/120。地层按计算深度定义。源为竖直线体力，位于计算深度 15 m；接收 81 道、X=80..400 m、间距 4 m、计算深度 1 m，记录间隔 0.5 ms。', '',
             '各工况按自己的稳定时间步推进到同一时长，再插值到共同记录轴。波场图标注各自的实际快照时间，不把不同 dt 的半程快照称为完全同一时刻。', '',
             '早/晚窗分别为 [0.1,0.3)、[0.3,0.6) s，是描述性窗口，不自动等同于反射波/面波或信噪比。未对道集施加 AGC、逐道归一化或后处理吸收滤波。频谱仅使用相同 Hann 窗。', '',
             'Q 更小通常产生更强材料耗散，但干涉、相位和波型转换可使局部道/波窗振幅不单调。真实资料还受速度、散射、源子波、耦合、仪器及三维效应影响，需独立标定和留出资料检验。', '']
    for name, title in zip(manifest['images'], ['解析基准与频散', '平地/起伏，有无 Q 的道集', '起伏地表 Q 敏感性', '物理坐标波场对比']):
        text += [f'## {title}', '', f'![{title}]({name}.png)', '']
    text += ['## 复现与文件', '', '运行 `python -B tools/reproduce_qp_qs.py --run --backend cuda`；仅重绘时不加 `--run`。原始模型、记录、波场及波窗 CSV 在各工况子目录；参数、结果和代码/数据哈希在 manifest.json。', '',
             '本目录的 [METHOD.md](METHOD.md) 包含方法与论文目录，[文献 BibTeX](references/attenuation.bib) 可导入 Zotero。物理与现场吻合度的证据应分别报告。', '']
    (folder / 'REPORT.md').write_text('\n'.join(text), encoding='utf-8')
    html = '<!doctype html><meta charset="utf-8"><title>二维 Qp/Qs 复现</title><style>body{max-width:1100px;margin:36px auto;font:17px/1.65 system-ui;color:#20303c}img{width:100%}h2{margin-top:40px}a{color:#27658a}</style><h1>二维 Qp/Qs 方法复现与受控模拟</h1><p>GSLS 方法、独立解析基准与自定义合成算例；尚无实测资料拟合验证。</p><p><a href="REPORT.md">完整说明与参数</a> · <a href="manifest.json">参数、结果与哈希清单</a></p>'
    for name in manifest['images']:
        html += f'<h2>{name.replace("_", " ")}</h2><img src="{name}.png">'
    (folder / 'index.html').write_text(html, encoding='utf-8')
    if args.archive_source:
        paths=subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=ROOT).split(b'\0')
        source=folder/'source-worktree.zip'
        with zipfile.ZipFile(source,'w',zipfile.ZIP_DEFLATED) as archive:
            for relative in sorted(set(p.decode('utf-8') for p in paths if p)):
                path=(ROOT/relative).resolve()
                if not path.is_relative_to(ROOT) or not path.is_file():raise ValueError(f'Invalid source path: {relative}')
                archive.write(path,'SeisWaveLab_Curvilinear/'+relative)
        manifest['source_snapshot']={'file':source.name,'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'scope':'Current non-ignored repository files, without Git history or external SDKs; build dependencies are documented in docs/BUILDING.md'}
        (folder/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
        bundle=folder.parent/(folder.name+'-results.zip')
        with zipfile.ZipFile(bundle,'w',zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(folder.rglob('*')):
                if path.is_file():archive.write(path,folder.name+'/'+path.relative_to(folder).as_posix())
        for archive in (source,bundle):
            with zipfile.ZipFile(archive) as checked:assert checked.testzip() is None
    print(json.dumps({'report': str(folder / 'REPORT.md'), 'validation': result, 'record_comparison': energy_rows}, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
