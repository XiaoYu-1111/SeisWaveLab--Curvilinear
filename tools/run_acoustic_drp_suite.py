"""Reproduce the local 15/30 Hz dry-loess pilot, one fresh CUDA process per case.

Generated models, raw Pa SEG-Y/CSV, benchmarks and logs stay in outer out/.
P1/DRP source/receiver physical coordinates, sample interval and absorber width
are identical within each frequency. No GPU case runs concurrently.
"""
from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / 'out'
DATA = OUT / 'validation/acoustic-drp-2026-10-31'

def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--run', action='store_true', help='Run missing cases; completed cases are retained')
    parser.add_argument('--references',action='store_true',help='Also run finer DRP references for the first 0.6 s')
    args = parser.parse_args()
    exe = OUT / 'bin/Release/seis2d_acoustic_drp_validation.exe'
    DATA.mkdir(parents=True, exist_ok=True)
    for frequency, coarse in [(15, 2.), (30, 1.)]:
        for h in [coarse, coarse / 2]:
            for method in [0, 1]:
                name = f'{frequency}Hz-{"DRP" if method else "P1"}-h{h:.6f}'
                command = [str(exe), '--experiment', str(frequency), str(method), str(h)]
                if (DATA / name / 'benchmark.csv').is_file():
                    print('Retained completed', name, flush=True)
                    continue
                print(' '.join(command), flush=True)
                if args.run:
                    with (DATA / (name + '.log')).open('w', encoding='utf-8') as log:
                        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
                    print('Completed', name, flush=True)
    if args.references:
        for frequency,h in [(15,.5),(30,.25)]:
            name=f'{frequency}Hz-DRP-h{h:.6f}-t0.600000'
            if (DATA/name/'benchmark.csv').is_file():
                print('Retained completed',name,flush=True)
                continue
            command=[str(exe),'--experiment',str(frequency),'1',str(h),'.6']
            print(' '.join(command),flush=True)
            if args.run:
                with (DATA/(name+'.log')).open('w',encoding='utf-8') as log:
                    subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
                print('Completed',name,flush=True)

if __name__ == '__main__':
    main()
