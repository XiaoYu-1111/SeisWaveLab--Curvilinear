"""Check the standalone source/documentation package without creating outputs."""
from pathlib import Path
import json
import re
from urllib.parse import unquote

ROOT=Path(__file__).resolve().parents[1]
errors=[]
pages=[ROOT/'README.md',ROOT/'THIRD_PARTY_NOTICES.md',*(ROOT/'docs').rglob('*.md')]
for page in pages:
    text=page.read_text(encoding='utf-8')
    text=re.sub(r'```[\s\S]*?```|`[^`\n]*`','',text)
    for target in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)',text):
        if target.startswith(('http:','https:','mailto:','#')):continue
        path=(page.parent/unquote(target.split('#')[0])).resolve()
        if not path.is_relative_to(ROOT) or not path.is_file():errors.append(f'{page.relative_to(ROOT)}: invalid repository link {target}')

presets=json.loads((ROOT/'CMakePresets.json').read_text(encoding='utf-8'))
for preset in presets['configurePresets']:
    if preset.get('hidden'):continue
    expected='${sourceDir}/../out/build/'+preset['name']
    if preset['binaryDir']!=expected:errors.append(f'Unexpected preset build path: {preset["name"]}')
settings=json.loads((ROOT/'CMakeSettings.json').read_text(encoding='utf-8'))
for setting in settings['configurations']:
    if setting['buildRoot']!='${projectDir}\\..\\out\\build\\${name}':errors.append('VS build path is not external')

for path in ROOT.rglob('*'):
    if '.git' in path.relative_to(ROOT).parts:continue
    if path==ROOT/'resources/examples/terrain3d/surface_grid_400x288_h25m.vts':continue  # Curated original default DEM, not a generated solver export.
    if path.is_file() and (path.suffix.lower() in {'.exe','.dll','.lib','.pdb','.obj','.ppm','.vts','.sgy','.segy','.zip','.pyc'} or path.name=='CMakeCache.txt'):
        errors.append(f'Generated file in repository: {path.relative_to(ROOT)}')

for name in ['LICENSE','cmake/Targets.cmake','resources/icons/seis3d.ico','docs/images/capture.tsv','third_party/cgfd3d_reference/fd_t.h']:
    if not (ROOT/name).is_file():errors.append(f'Missing standalone source resource: {name}')
if errors:raise SystemExit('\n'.join(errors))
print(f'PASS: {len(pages)} Markdown pages, repository-local links, external build presets, required resources, no generated binaries/caches')
