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
    layout_name=resolved.get('journal_layout')
    if layout_name is not None:
        layouts=catalog.get('journal_layouts',{})
        if layout_name not in layouts:raise ValueError(f'Unknown journal_layout {layout_name!r}; use {list(layouts)}')
        layout=merge(layouts[layout_name],resolved.get('journal_options',{}))
        for k in ('width_mm','font_size_pt','minimum_font_pt','maximum_font_pt','dpi'):
            if not isinstance(layout.get(k),(int,float)) or not math.isfinite(layout[k]) or layout[k]<=0:raise ValueError(f'journal_options.{k} must be finite and positive')
        if layout['minimum_font_pt']>layout['maximum_font_pt']:raise ValueError('journal font limits are reversed')
        resolved['journal_options']=layout
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

def prepare_journal_figure(fig, cfg):
    """Size at intended print width, preserving data, artist colours and norms.

    The layout is opt-in; individual panel design and legibility still require review.
    """
    if not cfg.get('journal_layout'):return False
    from matplotlib.text import Text
    layout=cfg['journal_options'];width=layout['width_mm']/25.4
    old_width,old_height=fig.get_size_inches()
    if old_width<=0 or old_height<=0:raise ValueError('Invalid figure size')
    fig.set_size_inches(width,width*old_height/old_width,forward=True)
    for text in fig.findobj(match=Text):
        if text.get_text():
            # Matplotlib may construct numeric tick labels only during the draw.
            text.set_fontsize(min(max(text.get_fontsize(),layout['minimum_font_pt']),layout['maximum_font_pt']))
    if fig.get_layout_engine() is None:
        fig.tight_layout()
    fig.canvas.draw()
    return True

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
