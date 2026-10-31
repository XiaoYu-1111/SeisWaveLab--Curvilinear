"""Publish verified native 2D module captures; no crop, resize or recoloring.

Run the acoustic/RTM UI smoke tests before this tool. Intermediate captures remain
in the outer out directory; only documentation resources are published here.
"""
from pathlib import Path
import hashlib
import json
import shutil
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / 'out'
CAPTURES = {
    'absorber-display/elastic.ppm': 'absorber-display/elastic.png',
    'absorber-display/viscoelastic.ppm': 'absorber-display/viscoelastic.png',
    'acoustic/forward-6.ppm': 'acoustic/workspace.png',
    'acoustic/model-wavefield-overlay.ppm': 'acoustic/model-wavefield-overlay.png',
    'acoustic/single-pressure-trace.ppm': 'acoustic/single-pressure-trace.png',
    'acoustic/absorber-wavefield.ppm': 'acoustic/absorber-wavefield.png',
    'acoustic/absorber-model-wavefield.ppm': 'acoustic/absorber-model-wavefield.png',
    'acoustic/floating-parameters.ppm': 'acoustic/floating-parameters.png',
    'acoustic/keyboard-help.ppm': 'acoustic/keyboard-help.png',
    'acoustic/terrain-acquisition.ppm': 'acoustic/terrain-acquisition.png',
    'acoustic/model-capacity.ppm': 'acoustic/model-capacity.png',
    'acoustic/shared-layer-editor.ppm': 'acoustic/shared-layer-editor.png',
    'acoustic/shared-terrain-editor.ppm': 'acoustic/shared-terrain-editor.png',
    'acoustic/surface-receivers.ppm': 'acoustic/surface-receivers.png',
    'acoustic/drp-workspace.ppm': 'acoustic/drp-workspace.png',
    'acoustic/auto-surface.ppm': 'acoustic/auto-surface.png',
    'auto-surface/elastic.ppm': 'auto-surface/elastic.png',
    'validation/acoustic-capacity-auto-2026-10-31/cuda-5000x2000.ppm': 'acoustic/cuda-5000x2000.png',
    'imaging/running-preview.ppm': 'imaging/running-preview.png',
    'imaging/rtm-6.ppm': 'imaging/workspace.png',
    'imaging/floating-parameters.ppm': 'imaging/floating-parameters.png',
    'imaging/rebuild-surface.ppm': 'imaging/rebuild-surface.png',
    'imaging/manual-receivers.ppm': 'imaging/manual-receivers.png',
}


def main():
    # Validate every input before publishing any asset.
    images = []
    for source, target in CAPTURES.items():
        path = OUT / source if source.startswith('validation/') else OUT / 'screenshots' / source
        with Image.open(path) as image:
            image.load()
            if image.size != (1600, 1000):
                raise ValueError(f'Unexpected native capture size: {path}: {image.size}')
            images.append((source, target, image.copy()))
    validation = OUT / 'validation/module-isolation/validation.txt'
    if not validation.read_text(encoding='utf-8').startswith('PASS:'):
        raise ValueError('Module isolation validation has not passed')
    interaction = OUT / 'validation/acoustic-2d/ui-interactions.txt'
    if not interaction.read_text(encoding='utf-8').startswith('PASS:'):
        raise ValueError('Acoustic keyboard/aspect/acquisition validation has not passed')
    evidenceFiles={}
    for filename in ['native-validation.txt','regression.txt','elastic-ui.txt','capture-regression.txt']:
        evidence=OUT/'validation/absorber-display-2026-10-31'/filename
        value=evidence.read_text(encoding='utf-8')
        passed='100% tests passed' in value if 'regression' in filename else value.startswith('PASS:')
        if not passed:
            raise ValueError(f'Absorber display validation is incomplete: {filename}')
        evidenceFiles[filename]=evidence
    data = ROOT / 'docs/data/module-ui-2026-10-30'
    data.mkdir(parents=True, exist_ok=True)
    manifest = []
    for source, target, image in images:
        destination = ROOT / 'docs/images' / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        image.save(destination)
        manifest.append(dict(source=f'out/{source}' if source.startswith('validation/') else f'out/screenshots/{source}', image=f'docs/images/{target}',
                             size=list(image.size), theme='Red Yellow Orange',
                             processing='Lossless PNG only; native complete framebuffer',
                             sha256=hashlib.sha256(destination.read_bytes()).hexdigest()))
    shutil.copy2(validation, data / 'validation.txt')
    shutil.copy2(interaction, data / 'acoustic-ui-interactions.txt')
    (data / 'screenshots.json').write_text(
        json.dumps(dict(date='2026-10-31', captures=manifest), indent=2) + '\n', encoding='utf-8')
    absorberData=ROOT/'docs/data/absorber-display-2026-10-31'
    absorberData.mkdir(parents=True,exist_ok=True)
    for filename,evidence in evidenceFiles.items():
        shutil.copy2(evidence,absorberData/filename)
    shutil.copy2(interaction,absorberData/'acoustic-ui.txt')
    workflowData=ROOT/'docs/data/workflow-upgrade-2026-10-31'
    workflowData.mkdir(parents=True,exist_ok=True)
    for filename in ['native.txt','regression-final.txt','preview-final.txt','capacity-guard.txt','elastic-workspace.txt']:
        evidence=OUT/'validation/workflow-upgrade-2026-10-31'/filename
        value=evidence.read_text(encoding='utf-8')
        if not ('PASS' in value or '100% tests passed' in value):
            raise ValueError(f'Workflow validation is incomplete: {filename}')
        shutil.copy2(evidence,workflowData/filename)
    shutil.copy2(interaction,workflowData/'acoustic-ui.txt')
    capacityData=ROOT/'docs/data/acoustic-capacity-auto-2026-10-31'
    capacityData.mkdir(parents=True,exist_ok=True)
    for filename in ['ui-capacity.txt','large-ui.txt','regression-first.txt','dropdown-refresh.txt','surface-regression.txt']:
        evidence=OUT/'validation/acoustic-capacity-auto-2026-10-31'/filename
        value=evidence.read_text(encoding='utf-8')
        if not ('PASS' in value or '100% tests passed' in value):
            raise ValueError(f'Capacity/surface validation is incomplete: {filename}')
        shutil.copy2(evidence,capacityData/filename)
    shutil.copy2(OUT/'validation/acoustic-cpml-2d/validation.txt',capacityData/'cpml-validation.txt')
    shutil.copy2(OUT/'validation/workflow-upgrade-2026-10-31/native.txt',capacityData/'workflow-validation.txt')
    shutil.copy2(interaction,capacityData/'ui-interactions.txt')
    rtmData=ROOT/'docs/data/rtm-survey-2026-10-31'
    rtmData.mkdir(parents=True,exist_ok=True)
    for source,target in [('validation/rtm-survey-final.txt','regression.txt'),
        ('validation/rtm-survey-2026-10-31/ui-interactions.txt','ui-interactions.txt'),
        ('validation/rtm-2d/validation.txt','native-validation.txt')]:
        evidence=OUT/source;value=evidence.read_text(encoding='utf-8')
        if not ('PASS' in value or '100% tests passed' in value):
            raise ValueError(f'RTM survey validation is incomplete: {source}')
        shutil.copy2(evidence,rtmData/target)
    print(f'Published {len(manifest)} native captures and module isolation evidence')
    autoData=ROOT/'docs/data/auto-surface-2026-10-31';autoData.mkdir(parents=True,exist_ok=True)
    for source,target in [('validation/auto-surface-final.txt','regression.txt'),
        ('validation/auto-surface-ui-final.txt','ui-final.txt'),
        ('validation/auto-surface-2026-10-31/native.txt','native.txt'),
        ('validation/auto-surface-dem.txt','dem-check.txt'),
        ('validation/auto-surface-2026-10-31/elastic-ui.txt','elastic-ui.txt'),
        ('validation/acoustic-2d/ui-interactions.txt','acoustic-ui.txt'),
        ('validation/auto-surface-elastic-full.txt','elastic-full.txt')]:
        evidence=OUT/source;value=evidence.read_text(encoding='utf-8')
        if not ('PASS' in value or '100% tests passed' in value):raise ValueError(f'Automatic survey validation is incomplete: {source}')
        shutil.copy2(evidence,autoData/target)


if __name__ == '__main__':
    main()
