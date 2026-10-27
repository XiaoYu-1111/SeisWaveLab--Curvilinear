"""Validate existing 15/30 Hz publication manifests without regenerating reports."""
import hashlib,json,math,sys
from pathlib import Path
sys.dont_write_bytecode=True
from seisplot.core import WORKSPACE
from seisplot.publication import resolve_style


def check():
    style=resolve_style({'style_profile':'jingbian_15_30'});snap=style['snapshots'];checked=[]
    for hz in [15,30]:
        root=WORKSPACE/f'out/jingbian_{hz}Hz_2m_analysis'
        manifest=json.loads((root/'data/revision_figure_manifest.json').read_text(encoding='utf-8'))
        rows=manifest['same_time_comparisons']
        if [r['target_time_s'] for r in rows]!=snap['times_s']:raise ValueError(f'{hz}Hz: snapshot times differ')
        for row in rows:
            if row['colormap']!=snap['cmap'] or row['interfaces_m']!=snap['reference_depths_m']:raise ValueError(f'{hz}Hz: display policy differs')
            p=root/'figures'/row['file']
            if not p.resolve().is_relative_to((root/'figures').resolve()):raise ValueError('Image path leaves figures directory')
            digest=hashlib.sha256(p.read_bytes()).hexdigest()
            if digest!=row['sha256']:raise ValueError(f'Image hash mismatch: {p}')
            checked.append(str(p.relative_to(WORKSPACE)))
        policy=manifest['snapshot_color_policy']
        if policy['component']!='Vz' or not math.isclose(policy['vmax'],snap['vmax_m_s']*1e6,rel_tol=1e-12) or not math.isclose(policy['linthresh'],snap['linthresh_m_s']*1e6,rel_tol=1e-12):raise ValueError(f'{hz}Hz: wavefield scale differs')
        if not manifest['model_colormap'].startswith(style['models']['cmap']):raise ValueError('Model colormap differs')
        record=json.loads((root/'data/record_display_policy.json').read_text(encoding='utf-8'))
        if record['display_colormap']!=style['record_cmap'] or not math.isclose(record['agc_window_s'],style['records']['agc_window_s'],rel_tol=1e-12) or record['agc_display_limit']!=style['records']['agc_clip']:raise ValueError('Record display policy differs')
    receipt={'status':'passed','checked_wavefield_images':len(checked),'style_provenance':style['style_provenance'],'files':checked}
    output=WORKSPACE/'out/validation/seismic_plotting/publication_verification.json';output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(receipt,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in receipt.items() if k!='files'},ensure_ascii=False,indent=2))
    return receipt


if __name__=='__main__':check()
