"""Publish reviewed acoustic-forward validation figures, source logs and preliminary cost samples."""
from pathlib import Path
import argparse
import csv
import json
import shutil
import statistics
import sys
import xml.etree.ElementTree as ET

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
    folder = OUT / 'validation/acoustic-2d'
    assert 'direct P arrival / zero-pressure top' in (folder / 'validation.txt').read_text()
    candidates = sorted(folder.glob('experiment-*'))
    source = next(p for p in reversed(candidates) if (p / 'COMPLETE.txt').exists())
    tree = ET.parse(source / 'pressure.vts')
    extent = list(map(int, tree.find('.//Piece').attrib['Extent'].split()))
    nx, nz = extent[1] + 1, extent[5] + 1
    pressure = np.fromstring(tree.find('.//PointData/DataArray[@Name="Pressure_Pa"]').text,
                             sep=' ').reshape(nz, nx)
    coordinates = np.fromstring(tree.find('.//Points/DataArray').text, sep=' ').reshape(nz, nx, 3)
    records = np.loadtxt(source / 'pressure_records.csv', delimiter=',', skiprows=2)
    peak = np.max(np.abs(pressure)) * .65
    expected = 1.5 / 20 + 100 / 1800
    window = np.abs(records[:, 0] - expected) < .025
    peak_time = records[window, 0][np.argmax(np.abs(records[window, 1]))]
    plt.rcParams.update({'font.family': ['Times New Roman'], 'font.size': 11,
                         'axes.linewidth': .65, 'xtick.direction': 'in', 'ytick.direction': 'in'})
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5), layout='constrained')
    im = axes[0].pcolormesh(coordinates[:, :, 0], coordinates[:, :, 2], pressure,
                            shading='nearest', cmap='coolwarm', vmin=-peak, vmax=peak, rasterized=True)
    axes[0].scatter([100], [100], marker='*', color='#c22823', s=70, edgecolor='black', linewidth=.5)
    axes[0].scatter([200], [100], marker='v', color='#183f73', s=35)
    axes[0].set(xlim=(0, 400), ylim=(400, 0), xlabel='X (m)', ylabel='Z positive down (m)',
                title='(a) Acoustic pressure: direct and free-surface reflected P')
    fig.colorbar(im, ax=axes[0], label='Pressure (Pa)', fraction=.046, pad=.025)
    axes[1].plot(records[:, 0], records[:, 1], color='#244e70', lw=.85)
    axes[1].axvline(expected, color='#d04a26', lw=.8, ls='--', label=f'Ricker centre + r/Vp: {expected:.4f} s')
    axes[1].axvline(peak_time, color='#254e9a', lw=.8, ls=':', label=f'Numerical peak: {peak_time:.4f} s')
    axes[1].set(xlabel='Time (s)', ylabel='Pressure (Pa)', title='(b) Receiver at X=200 m, Z=100 m')
    axes[1].grid(alpha=.14, lw=.5)
    axes[1].legend(frameon=False, fontsize=9)
    fig.suptitle('Forward acoustic validation | Vp=1800 m/s | h=5 m | 20 Hz | no S-wave equations', fontsize=12)
    figures = folder / 'figures'
    figures.mkdir(exist_ok=True)
    fig.savefig(figures / 'pressure_validation.png', dpi=220)
    fig.savefig(figures / 'pressure_validation.pdf')
    plt.close(fig)
    with (folder / 'mode_cost.csv').open(newline='') as f:
        rows = list(csv.DictReader(f))
    groups = {}
    for row in rows:
        key = (int(row['nx']), int(row['nz']), row['backend'], row['mode'])
        groups.setdefault(key, []).append(row)
    summary = []
    for (nx, nz, backend, mode), group in groups.items():
        values = [float(v['compute_ms']) for v in group]
        summary.append(dict(nx=nx, nz=nz, backend=backend, mode=mode, repeats=len(group),
                            compute_ms_median=statistics.median(values),
                            compute_ms_min=min(values), compute_ms_max=max(values),
                            device_arrays_bytes=int(group[0]['device_arrays_bytes']),
                            caution='Unwarmed short software-path comparison; not equal-error/fidelity or sustained performance.'))
    (folder / 'cost_summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    if args.publish:
        images = ROOT / 'docs/images/acoustic'
        data = ROOT / 'docs/data/acoustic-2d-2026-10-30'
        images.mkdir(parents=True, exist_ok=True)
        data.mkdir(parents=True, exist_ok=True)
        shutil.copy2(figures / 'pressure_validation.png', images / 'pressure.png')
        for name in ['validation.txt', 'capacity.txt', 'mode_cost.csv', 'cost_summary.json']:
            shutil.copy2(folder / name, data / name)
        from PIL import Image
        Image.open(OUT / 'screenshots/acoustic/forward-6.ppm').save(images / 'workspace.png')
    print(figures)


if __name__ == '__main__':
    main()
