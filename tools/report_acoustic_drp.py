"""Plot verified DRP evidence and the local dry-loess pilot; all results in outer out.

Requires numpy/matplotlib. No AGC, trace normalization, alignment, amplitude fitting
or implicit rescaling is applied to the numerical error calculation.
"""
from pathlib import Path
import argparse
import csv
import hashlib
import json
import re
import shutil
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT.parent / 'out/validation/acoustic-drp-2026-10-31'

def result_dir(case):
    pointer = case / 'result-path.txt'
    return case / (pointer.read_text(encoding='utf-8').strip() if pointer.is_file() else 'result')

def records(case):
    data = np.loadtxt(result_dir(case) / 'pressure_records.csv', delimiter=',', skiprows=2)
    return data[:, 0], data[:, 1:]

def row(case):
    return next(csv.DictReader((case / 'benchmark.csv').open(encoding='utf-8')))

def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('--publish',action='store_true',help='Publish verified figures and compact evidence into docs')
    args=parser.parse_args()
    plt.rcParams.update({'font.family':'serif','font.serif':['Times New Roman','DejaVu Serif'],
                         'font.size':10,'axes.linewidth':.7,'lines.linewidth':1.0,
                         'savefig.dpi':220,'axes.spines.top':False,'axes.spines.right':False})
    validation=(DATA/'validation.txt').read_text(encoding='utf-8')
    if not re.search(r'^PASS:',validation,re.M): raise ValueError('DRP native validation has not passed')
    fig,axes=plt.subplots(1,3,figsize=(12,3.4),layout='constrained')
    modes=list(csv.DictReader((DATA/'analytic_modes.csv').open(encoding='utf-8')))
    for method,color in [('P1','#265b8b'),('DRP7','#c53b23')]:
        rows=[r for r in modes if r['method']==method]
        axes[0].loglog([float(r['h']) for r in rows],[float(r['L2_initial_normalized']) for r in rows],
                      'o-',color=color,label=method,markersize=4)
    axes[0].set(xlabel='Grid interval h (m)',ylabel='Initial-amplitude-normalized L2 error',title='Analytic closed-box mode')
    axes[0].legend(frameon=False);axes[0].grid(alpha=.16)
    reflection=np.loadtxt(DATA/'normal_reflection.csv',delimiter=',',skiprows=1)
    axes[1].plot(reflection[:,0],reflection[:,1],color='#265b8b')
    axes[1].plot([.25],[.29411764705882],'o',color='#c53b23',label='Theoretical reflection at 0.25 s',markersize=4)
    axes[1].set(xlabel='Time (s)',ylabel='Pressure (Pa)',title='Normal incidence: density contrast')
    axes[1].legend(frameon=False,fontsize=8)
    residuals=[float(x) for x in re.findall(r'target_residual_energy.*?ratio=([0-9.eE+-]+)',validation)]
    axes[2].bar(['Rigid outer','Flat CPML','Terrain CPML'],residuals,color=['#777777','#e68b2f','#c53b23'])
    axes[2].set(yscale='log',ylabel='Target energy / initial target energy',title='Late-time energy (1.5 s)')
    fig.savefig(DATA/'verification.png');plt.close(fig)

    summary=[];fig,axes=plt.subplots(2,3,figsize=(12,6.3),layout='constrained')
    shot,sa=plt.subplots(2,3,figsize=(12,7),layout='constrained',sharex=True,sharey=True)
    for fi,(frequency,coarse) in enumerate([(15,2.),(30,1.)]):
        refh=coarse/4
        reference=DATA/f'{frequency}Hz-DRP-h{refh:.6f}-t0.600000'
        rt,ref=records(reference)
        common=(rt<=.6+1e-12)
        rt,ref=rt[common],ref[common]
        finest=DATA/f'{frequency}Hz-DRP-h{coarse/2:.6f}'
        ft,fine_records=records(finest)
        datasets={}
        for method in ['P1','DRP']:
            for h in [coarse,coarse/2]:
                case=DATA/f'{frequency}Hz-{method}-h{h:.6f}'
                t,a=records(case)
                interpolated=np.column_stack([np.interp(rt,t,a[:,r]) for r in range(a.shape[1])])
                nrmse=float(np.sqrt(np.sum((interpolated-ref)**2)/np.sum(ref**2)))
                gain=float(np.sqrt(np.sum(interpolated**2)/np.sum(ref**2)))
                # Central trace lag is diagnostic only: the errors above stay unaligned.
                lag_scores=[]
                for lag in range(-20,21):
                    x,y=interpolated[:,10],ref[:,10]
                    if lag>0:x,y=x[lag:],y[:-lag]
                    if lag<0:x,y=x[:lag],y[-lag:]
                    lag_scores.append(float(np.sum(x*y)))
                lag=(int(np.argmax(lag_scores))-20)*.5
                fine=np.column_stack([np.interp(t,ft,fine_records[:,r]) for r in range(a.shape[1])])
                r=row(case)
                r.update(NRMSE_0p6s=nrmse,amplitude_norm_ratio=gain,central_trace_lag_ms=lag,
                         L2_difference_to_3s_fine_DRP=float(np.sqrt(np.sum((a-fine)**2)/np.sum(fine**2))),
                         reference_h=refh,reference_window_s=.6)
                summary.append(r);datasets[(method,h)]=(t,a)
        for method,color in [('P1','#265b8b'),('DRP7','#c53b23')]:
            data=[r for r in summary if int(r['frequency'])==frequency and r['method']==method]
            axes[fi,0].plot([float(r['h']) for r in data],[r['NRMSE_0p6s'] for r in data],'o-',color=color,label=method)
            axes[fi,1].plot([r['NRMSE_0p6s'] for r in data],[float(r['compute_ms'])/1000 for r in data],'o-',color=color,label=method)
            axes[fi,2].plot([float(r['h']) for r in data],[float(r['device_array_bytes'])/2**20 for r in data],'o-',color=color,label=method)
        axes[fi,0].set(title=f'{frequency} Hz: raw pressure error (0-0.6 s)',xlabel='h (m)',ylabel='NRMSE')
        axes[fi,1].set(title=f'{frequency} Hz: 3 s CUDA propagation',xlabel='NRMSE (0-0.6 s)',ylabel='Compute time (s)',yscale='log')
        axes[fi,2].set(title=f'{frequency} Hz: device arrays',xlabel='h (m)',ylabel='MiB')
        for ax in axes[fi]:ax.legend(frameon=False);ax.grid(alpha=.15)
        limit=float(np.percentile(np.abs(ref),99.5))
        for ci,(label,t,a) in enumerate([
            ('P1 coarse',*datasets[('P1',coarse)]),('DRP7 coarse',*datasets[('DRP',coarse)]),
            ('DRP7 finer reference',rt,ref)]):
            keep=t<=.6+1e-12
            im=sa[fi,ci].imshow(a[keep],origin='upper',aspect='auto',cmap='gray',vmin=-limit,vmax=limit,
                 interpolation='bilinear',extent=[.1,.5,float(t[keep][-1]),float(t[keep][0])])
            sa[fi,ci].set(title=f'{frequency} Hz | {label}',xlabel='Receiver X (km)',ylabel='Time (s)')
        shot.colorbar(im,ax=sa[fi,:],label='Pressure (Pa); common scale per frequency',fraction=.018,pad=.02)
    fig.savefig(DATA/'pilot_efficiency.png');shot.savefig(DATA/'pilot_records.png');plt.close(fig);plt.close(shot)
    with (DATA/'pilot_summary.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=list(summary[0]));writer.writeheader();writer.writerows(summary)
    acceptance={}
    for frequency in [15,30]:
        acceptance[str(frequency)]={method:[float(r['h']) for r in summary if int(r['frequency'])==frequency and
                                      r['method']==method and r['NRMSE_0p6s']<=.05] for method in ['P1','DRP7']}
    (DATA/'report.json').write_text(json.dumps(dict(date='2026-10-31',cases=summary,
        acceptance_5pct_raw_pressure=acceptance,notes=[
        'Finer DRP numerical references cover 0-0.6 s only; not analytic truth.',
        '3 s runs verify bounded local pilot behavior, not whole-work-area qualification.',
        'P1 nodal and DRP 5x5 binomial sources have the same continuum normalization but different finite-grid footprints.',
        'Each timing is a single fresh-process run, no universal speedup claim.',
        'Device arrays exclude CUDA context. Peak working set includes CUDA/runtime/export allocations.',
        'No alignment, AGC or amplitude fitting in error computation.']),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(acceptance,ensure_ascii=False))
    if args.publish:
        evidence=ROOT/'docs/data/acoustic-drp-2026-10-31'
        images=ROOT/'docs/images/acoustic-drp'
        evidence.mkdir(parents=True,exist_ok=True);images.mkdir(parents=True,exist_ok=True)
        final=(DATA.parent/'drp-regression-final.txt').read_text(encoding='utf-8')
        if '100% tests passed' not in final:raise ValueError('DRP regression has not passed')
        manifest=[]
        for name in ['verification.png','pilot_efficiency.png','pilot_records.png']:
            target=images/name;shutil.copy2(DATA/name,target)
            manifest.append(dict(file=target.relative_to(ROOT).as_posix(),sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
        for name in ['validation.txt','analytic_modes.csv','normal_reflection.csv','pilot_summary.csv','report.json']:
            shutil.copy2(DATA/name,evidence/name)
        shutil.copy2(DATA.parent/'drp-regression-final.txt',evidence/'regression.txt')
        shutil.copy2(DATA.parent/'drp-final-check.txt',evidence/'final-check.txt')
        shutil.copy2(DATA.parent/'acoustic-2d/ui-interactions.txt',evidence/'ui-interactions.txt')
        for path in sorted(DATA.glob('*/benchmark.csv')):
            case=path.parent;target=evidence/'cases'/case.name;target.mkdir(parents=True,exist_ok=True)
            shutil.copy2(path,target/'benchmark.csv')
            shutil.copy2(result_dir(case)/'experiment.txt',target/'experiment.txt')
        (evidence/'figures.json').write_text(json.dumps(dict(date='2026-10-31',figures=manifest),indent=2)+'\n',encoding='utf-8')
        print('Published verified figures and compact DRP evidence')

if __name__=='__main__':
    main()
