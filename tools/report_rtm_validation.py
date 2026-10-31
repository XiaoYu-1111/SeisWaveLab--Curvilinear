"""Render the native acoustic RTM acceptance fixture; generated outputs stay in outer out/."""
from pathlib import Path
import argparse
import shutil
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
    folder = OUT / 'validation/rtm-2d'
    experiments = sorted(folder.glob('experiment-*'))
    source = next(p for p in reversed(experiments) if (p / 'experiment.txt').is_file())
    tree = ET.parse(source / 'rtm.vts')
    extent = list(map(int, tree.find('.//Piece').attrib['Extent'].split()))
    nx, nz = extent[1] + 1, extent[5] + 1
    values = {p.attrib['Name']: np.fromstring(p.text, sep=' ').reshape(nz, nx)
              for p in tree.findall('.//PointData/DataArray')}
    coordinates = np.fromstring(tree.find('.//Points/DataArray').text, sep=' ').reshape(nz, nx, 3)
    x, z = coordinates[:, :, 0], coordinates[:, :, 2]
    plt.rcParams.update({'font.family': ['Times New Roman'], 'font.size': 10,
                         'axes.linewidth': .65, 'xtick.direction': 'in', 'ytick.direction': 'in'})
    fig, axs = plt.subplots(2, 2, figsize=(12, 7), layout='constrained')
    for ax, name, title, cmap in zip(axs.flat,
            ['Vp_true', 'Vp_background', 'RTM_raw', 'Source_illumination'],
            ['(a) True Vp', '(b) Background Vp', '(c) RTM: zero-lag correlation', '(d) Source illumination'],
            ['turbo', 'turbo', 'coolwarm', 'turbo']):
        a = values[name]
        if name.startswith('Vp'):
            low, high, unit = 1800, 2300, 'Vp (m/s)'
        elif name == 'RTM_raw':
            high = np.max(np.abs(a)) * .65
            low, unit = -high, 'Arbitrary image scale'
        else:
            a = np.log10(np.maximum(a / np.max(a), 1e-8))
            low, high, unit = -6, 0, r'$\log_{10}(I/I_{max})$'
        im = ax.pcolormesh(x, z, a, shading='nearest', cmap=cmap, vmin=low, vmax=high, rasterized=True)
        ax.axhline(200, color='#262626', ls='--', lw=.65)
        ax.set(xlabel='X (m)', ylabel='Z positive down (m)', title=title, xlim=(0, 600), ylim=(400, 0))
        fig.colorbar(im, ax=ax, label=unit, fraction=.047, pad=.025)
    fig.suptitle('Acoustic RTM acceptance fixture | 3 shots | 15 Hz | h = 5 m\n'
                 'Dashed line: true interface at 200 m; strongest central-aperture image band at 185 m', fontsize=12)
    destination = folder / 'figures'
    destination.mkdir(exist_ok=True)
    fig.savefig(destination / 'rtm_benchmark.png', dpi=220)
    fig.savefig(destination / 'rtm_benchmark.pdf')
    plt.close(fig)
    if args.publish:
        images = ROOT / 'docs/images/imaging'
        data = ROOT / 'docs/data/rtm-2d-2026-10-30'
        images.mkdir(parents=True, exist_ok=True)
        data.mkdir(parents=True, exist_ok=True)
        shutil.copy2(destination / 'rtm_benchmark.png', images / 'benchmark.png')
        shutil.copy2(folder / 'validation.txt', data / 'validation.txt')
        shutil.copy2(source / 'experiment.txt', data / 'experiment.txt')
        # Full simulation arrays and raw captures remain outside the source repository.
        from PIL import Image
        Image.open(OUT / 'screenshots/imaging/rtm-6.ppm').save(images / 'workspace.png')
    print(destination)


if __name__ == '__main__':
    main()
