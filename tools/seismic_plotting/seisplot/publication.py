"""One versioned display policy. Numerical inputs and analysis windows are not modified."""
from copy import deepcopy
from pathlib import Path
import hashlib,json,math

STYLE_FILE=Path(__file__).resolve().parents[1]/'styles/current_publication.json'

def style_catalog():
    return json.loads(STYLE_FILE.read_text(encoding='utf-8'))

def merge(base,overrides):
    result=deepcopy(base)
    for key,value in overrides.items():
        result[key]=merge(result[key],value) if isinstance(value,dict) and isinstance(result.get(key),dict) else deepcopy(value)
    return result

def resolve_style(cfg):
    catalog=style_catalog();name=cfg.get('style_profile','publication')
    if name not in catalog['profiles']:raise ValueError(f'Unknown style_profile {name!r}; use {list(catalog["profiles"])}')
    colors=catalog['colors']
    common=merge(catalog['common'],{'record_cmap':colors['record'],'models':{'cmap':colors['model']},'snapshots':{'cmap':colors['wavefield']},'dispersion':{'cmap':colors['dispersion']}})
    resolved=merge(merge(common,catalog['profiles'][name]),cfg)
    resolved['style_profile']=name
    resolved['style_provenance']={'version':catalog['version'],'file':'styles/current_publication.json','sha256':hashlib.sha256(STYLE_FILE.read_bytes()).hexdigest()}
    for key in ['agc_window_s','agc_clip','raw_limit_m_s']:
        value=resolved.get('records',{}).get(key)
        if value is not None and (not math.isfinite(value) or value<=0):raise ValueError(f'{key} must be finite and positive')
    floor=resolved.get('records',{}).get('agc_floor',.01)
    if not math.isfinite(floor) or not 0<=floor<=1:raise ValueError('agc_floor must be in [0,1]')
    snap=resolved.get('snapshots',{});scope=snap.get('scale_scope','global')
    if scope not in ['global','per_time','fixed']:raise ValueError('scale_scope: global/per_time/fixed')
    if scope=='fixed' and (not math.isfinite(snap.get('vmax_m_s',0)) or snap.get('vmax_m_s',0)<=0):raise ValueError('fixed scale requires positive vmax_m_s')
    if snap.get('linthresh_m_s') is not None and not 0<snap['linthresh_m_s']<snap.get('vmax_m_s',float('inf')):raise ValueError('linthresh_m_s must be positive and below vmax_m_s')
    return resolved

def ordered_components(available,preferred=('Vz','Vx')):
    available=set(available)
    return [c for c in preferred if c in available]+sorted(available-set(preferred))

def velocity_scale(unit):
    if unit=='m/s':return 1.,'m/s'
    if unit in ['um/s','µm/s','μm/s']:return 1e6,'µm/s'
    raise ValueError('velocity unit must be m/s or um/s')

def reference_lines(ax,depths,options=None):
    options=options or {}
    # These markers denote physical depths. They must not be reused as local burial/eta.
    if options.get('depth_coordinate','physical')!='physical':return []
    style=style_catalog()['common']['reference_lines'];lo,hi=sorted(ax.get_ylim());artists=[]
    import matplotlib.patheffects as pe
    for depth in depths:
        if not lo<=depth<=hi:continue
        line=ax.axhline(depth,color=style['color'],lw=style['linewidth'],ls=(0,(6,4)),alpha=style['alpha'],zorder=3,
            path_effects=[pe.Stroke(linewidth=1.2,foreground='white',alpha=.45),pe.Normal()])
        label=ax.text(.985,depth-(hi-lo)*.01,f'{depth:g} m',transform=ax.get_yaxis_transform(),ha='right',va='bottom',fontsize=style['label_size'],color='#303840',zorder=4,
            bbox={'facecolor':'white','edgecolor':'none','alpha':.82,'pad':1.1})
        artists.extend([line,label])
    return artists
