"""Plot the native acoustic CPML/reference-domain audit; publish reviewed evidence with --publish."""
from pathlib import Path
import argparse
import csv
import json
import shutil
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / 'out'
sys.path.insert(0, str(ROOT / 'tools/seismic_plotting'))
from seisplot.runtime import prepare_runtime
prepare_runtime()
import numpy as np
import matplotlib
matplotlib.use('Agg')
from matplotlib import pyplot as plt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    source = OUT / 'validation/acoustic-cpml-2d'
    rows = list(csv.DictReader((source / 'reflection.csv').open(encoding='utf-8')))
    if len(rows) != 6 or any(float(r['improvement_db']) <= 6 or float(r['cpml_relative_l2']) >= .05 for r in rows):
        raise ValueError('The complete six-case CPML audit has not passed')
    if 'PASS: acoustic weak-form CFS-CPML reflection audit' not in (source / 'validation.txt').read_text():
        raise ValueError('Native CPML acceptance log is incomplete')
    plt.rcParams.update({'font.family': ['Times New Roman'], 'font.size': 11,
                         'axes.linewidth': .65, 'xtick.direction': 'in', 'ytick.direction': 'in'})
    fig = plt.figure(figsize=(12, 6.8), layout='constrained')
    grid = fig.add_gridspec(2, 2, width_ratios=(1, 1.12))
    ax = fig.add_subplot(grid[:, 0])
    positions = np.arange(6)
    ax.bar(positions - .18, [float(r['sponge_relative_l2']) for r in rows], .36,
           color='#d66032', label='Legacy sponge')
    ax.bar(positions + .18, [float(r['cpml_relative_l2']) for r in rows], .36,
           color='#265b94', label='CFS-CPML')
    ax.set(yscale='log', ylim=(3e-6, .2), ylabel='Relative record residual L2 (all 3 receivers)',
           title='(a) Equal 24-cell exterior width')
    ax.set_xticks(positions, ['Uniform', 'Shallow\nlarge offset', 'Corner', 'Terrain', 'Layered', 'Near-bottom\noblique'])
    ax.tick_params(axis='x', labelsize=10)
    ax.grid(axis='y', which='major', alpha=.15, lw=.5)
    ax.legend(frameon=False)
    for i, row in enumerate(rows):
        ax.text(i, float(row['sponge_relative_l2']) * 1.35,
                f"{float(row['improvement_db']):.1f} dB", ha='center', fontsize=10)
    for slot, case, caption in [(grid[0, 1], 'homogeneous', '(b) Uniform: R1 boundary residual'),
                                 (grid[1, 1], 'terrain', '(c) Terrain: R1 boundary residual')]:
        curve = np.genfromtxt(source / f'{case}_records.csv', delimiter=',', names=True)
        panel = fig.add_subplot(slot)
        scale = max(np.max(np.abs(curve['reference_R0'])), 1e-30)
        for key, label, color in [('sponge_R0', 'Legacy sponge', '#d66032'),
                                  ('cpml_R0', 'CFS-CPML', '#265b94')]:
            panel.plot(curve['time_s'], (curve[key] - curve['reference_R0']) / scale,
                       lw=.8, color=color, label=label)
        panel.set(xlabel='Time (s)', ylabel='Residual / reference peak', title=caption, xlim=(0, 1))
        panel.grid(alpha=.15, lw=.5)
        panel.legend(frameon=False, fontsize=9)
    fig.suptitle('Acoustic P1 FEM | exterior CFS-CPML vs legacy damping\n'
                 'Difference from a larger domain; these residuals are not the prescribed CPML reflection coefficient',
                 fontsize=12)
    figures = source / 'figures'
    figures.mkdir(exist_ok=True)
    fig.savefig(figures / 'reflection_comparison.png', dpi=250)
    fig.savefig(figures / 'reflection_comparison.pdf')
    plt.close(fig)
    summary = dict(cases=rows, metric='sqrt(sum((test-reference)^2)/sum(reference^2)), all 3 records, 0..1 s',
                   improvement_db='20 log10(sponge residual / CPML residual)',
                   reference='240-cell lateral margins and 240 extra bottom cells, chosen to delay physical outer-boundary returns beyond the observation interval',
                   limitation='Controlled 800x600 m fixtures, not a guarantee for arbitrary terrain/materials/frequency')
    (source / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    if args.publish:
        data = ROOT / 'docs/data/acoustic-cpml-2026-10-30'
        images = ROOT / 'docs/images/acoustic'
        data.mkdir(parents=True, exist_ok=True)
        images.mkdir(parents=True, exist_ok=True)
        for filename in ['reflection.csv', 'validation.txt', 'summary.json']:
            shutil.copy2(source / filename, data / filename)
        for filename in ['final-regression.txt', 'reference-time-axis-check.txt',
                         'elastic-ui-regression.txt', 'router-regression.txt',
                         'absorber-display-regression.txt']:
            evidence = source / filename
            if evidence.is_file():
                shutil.copy2(evidence, data / filename)
        for row in rows:
            shutil.copy2(source / f"{row['case']}_records.csv", data / f"{row['case']}_records.csv")
        shutil.copy2(OUT / 'validation/acoustic-2d/capacity.txt', data / 'capacity-cpml.txt')
        shutil.copy2(figures / 'reflection_comparison.png', images / 'cpml-comparison.png')
    print(figures)


if __name__ == '__main__':
    main()
