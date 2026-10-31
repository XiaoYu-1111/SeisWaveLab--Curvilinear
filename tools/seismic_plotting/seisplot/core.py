from pathlib import Path
import csv,json,math,warnings
import numpy as np
from scipy.ndimage import uniform_filter1d
from matplotlib import pyplot as plt, font_manager
from matplotlib.colors import SymLogNorm
from .io import metadata,read_segy,read_vts,table
from .publication import resolve_style,velocity_scale,reference_lines,prepare_journal_figure

REPO=Path(__file__).resolve().parents[3]
WORKSPACE=REPO.parent
PALETTE=['#c00000','#ed7d31','#744c91','#333333','#aa7942','#777777']

def resolve(path,base=WORKSPACE):
    p=Path(path).expanduser();return p.resolve() if p.is_absolute() else (Path(base)/p).resolve()

def load_config(path):
    return resolve_style(json.loads(Path(path).read_text(encoding='utf-8-sig')))

def setup(cfg):
    """All generated artifacts must stay in this checkout's outer out directory."""
    root=resolve(cfg['output_dir']);allowed=(WORKSPACE/'out').resolve()
    if not root.is_relative_to(allowed) or root==allowed:raise ValueError(f'output_dir must be a subdirectory of {allowed}')
    inputs=[]
    if cfg.get('input_dir'):inputs.append(resolve(cfg['input_dir']))
    for dataset in cfg.get('datasets',[]):
        for case in dataset['cases']:inputs.append(resolve(case.get('folder','.'),resolve(dataset['root'])))
    if any(root.is_relative_to(p) for p in inputs):raise ValueError('output_dir must not be inside an input case/inversion directory')
    for p in ['figures','data','cache']: (root/p).mkdir(parents=True,exist_ok=True)
    requested=(['Times New Roman','SimSun'] if cfg.get('journal_layout') else cfg.get('font_family',['Times New Roman','Microsoft YaHei']));requested=[requested] if isinstance(requested,str) else requested
    family=[f for f in requested if f in {v.name for v in font_manager.fontManager.ttflist}]
    if not family:
        candidates=['Noto Sans CJK SC','SimHei','DejaVu Sans']
        family=[next((x for x in candidates if x in {f.name for f in font_manager.fontManager.ttflist}),'DejaVu Sans')]
        warnings.warn(f'Requested font unavailable; using {family}')
    journal=cfg.get('journal_options',{}) if cfg.get('journal_layout') else {}
    plt.rcParams.update({'font.family':family,'font.size':journal.get('font_size_pt',cfg.get('font_size',14)),'axes.titlesize':journal.get('font_size_pt',cfg.get('title_size',16)),'axes.unicode_minus':False,
                         'axes.linewidth':cfg.get('axes_linewidth',.6),'axes.spines.top':True,'axes.spines.right':True,
                         'xtick.direction':'in','ytick.direction':'in','savefig.facecolor':'white','pdf.fonttype':42,'ps.fonttype':42,'svg.fonttype':'none'})
    (root/'data/config_used.json').write_text(json.dumps(cfg,ensure_ascii=False,indent=2,default=str),encoding='utf-8')
    return root

def save(fig,out,name,cfg):
    name=''.join(c if c.isalnum() or c in '_-.' else '_' for c in name)
    journal=prepare_journal_figure(fig,cfg)
    files=[]
    for ext in cfg.get('formats',['png','pdf']):
        if ext not in ['png','pdf','svg']:raise ValueError('formats: use png/pdf/svg')
        p=Path(out)/'figures'/f'{name}.{ext}'
        dpi=cfg['journal_options']['dpi'] if journal else cfg.get('dpi',180)
        fig.savefig(p,dpi=dpi,bbox_inches=None if journal else 'tight');files.append(p)
    plt.close(fig);return files

def write_rows(path,rows):
    if not rows:return
    with Path(path).open('w',newline='',encoding='utf-8-sig') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)

def case_paths(dataset,case):
    root=resolve(dataset['root']);folder=resolve(case.get('folder','.'),root)
    def item(key,default):return resolve(case[key],folder) if case.get(key) else folder/default
    meta=metadata(item('metadata_json','run_metadata.json'));meta.update(case.get('metadata',{}))
    return dict(case=case,folder=folder,meta=meta,model=item('model_vts','model/model.vts'),
                geometry=item('geometry_csv','records/receiver_geometry.csv'),
                components={c:resolve(p,folder) for c,p in case.get('record_files',{'Vx':'records/Vx.sgy','Vz':'records/Vz.sgy'}).items()},
                snapshot_index=item('snapshot_index_csv','snapshots/times.csv'),
                snapshot_dir=item('snapshot_dir','snapshots'))

def load_cases(dataset):return [case_paths(dataset,c) for c in dataset['cases']]

def records(case,component):
    meta=case['meta'];g=case['geometry'] if case['geometry'].is_file() else None
    return read_segy(case['components'][component],g,case['case'].get('sample_interval_s'),meta.get('source_x_m',0))

def gain(a,dt,mode='raw',window_s=.3,floor=.01,edge='truncated',odd=False,global_rms=None):
    a=np.asarray(a);rms=np.sqrt(np.mean(a*a)) if global_rms is None else global_rms
    if mode=='raw':return a.copy()
    if mode=='rms':return a/max(rms,np.finfo(float).tiny)
    if mode!='agc':raise ValueError('gain mode must be raw/rms/agc')
    n=max(1,int(round(window_s/dt)));n+=int(odd and n%2==0)
    if edge=='truncated':
        count=uniform_filter1d(np.ones(a.shape[-1]),n,mode='constant')
        local=np.sqrt(np.maximum(uniform_filter1d(a*a,n,axis=-1,mode='constant')/count,0));limit=floor*rms
    elif edge=='reflect':
        local=np.sqrt(np.maximum(uniform_filter1d(a*a,n,axis=-1,mode='reflect'),0));limit=floor*local.max()
    else:raise ValueError('AGC edge must be truncated or reflect')
    return a/np.maximum(local,max(limit,np.finfo(float).tiny))

def axes_grid(n,cols=2,height=3.5):
    cols=min(cols,n);rows=math.ceil(n/cols)
    fig,axes=plt.subplots(rows,cols,figsize=(6.2*cols,height*rows),layout='constrained',squeeze=False)
    for ax in axes.flat[n:]:ax.set_visible(False)
    return fig,list(axes.flat[:n])

def gather_image(ax,g,a,title,cfg,limit,relative=False,mode='raw'):
    opt=cfg.get('records',{});unit=opt.get('distance_unit','m');distance_scale=.001 if unit=='km' else 1.
    if unit not in ['m','km']:raise ValueError('distance_unit must be m or km')
    scale,_=velocity_scale(opt.get('velocity_unit','m/s')) if mode=='raw' else (1.,'RMS')
    xx=g.offset if relative else g.x
    order=np.argsort(xx);xx=xx[order]*distance_scale;a=a[order]*scale;limit=limit*scale
    if len(xx)>2 and not np.allclose(np.diff(xx),np.median(np.diff(xx))):
        # pcolormesh respects irregular receiver positions; imshow would mislabel them.
        im=ax.pcolormesh(xx,g.t,a.T,shading='auto',cmap=cfg.get('record_cmap','gray'),vmin=-limit,vmax=limit,rasterized=True)
    else:
        dx=np.median(np.diff(xx)) if len(xx)>1 else 1
        im=ax.imshow(a.T,extent=(xx[0]-dx/2,xx[-1]+dx/2,g.t[-1]+g.dt/2,-g.dt/2),aspect='auto',
                     cmap=cfg.get('record_cmap','gray'),vmin=-limit,vmax=limit,interpolation=cfg.get('interpolation','bilinear'),interpolation_stage=cfg.get('interpolation_stage','data'))
    ax.set(xlabel=('偏移距 / ' if relative else '接收点 X / ')+unit,ylabel='时间 / s',title=title,ylim=(g.t[-1],0))
    return im

def grid_data(case,field,out,options,snapshot=None):
    sx=options.get('stride_x',1);sz=options.get('stride_z',1)
    values=read_vts(snapshot or case['model'],field,sx,sz,Path(out)/'cache')
    coords=read_vts(case['model'],'Coordinates',sx,sz,Path(out)/'cache')['values']
    if coords.shape[:2]!=values['values'].shape:raise ValueError('Model/snapshot extent mismatch')
    x=coords[:,:,0].copy();z=coords[:,:,2].copy()
    if options.get('relative_x',False):x-=case['meta'].get('source_x_m',0)
    coordinate=options.get('depth_coordinate','physical')
    if coordinate=='eta':
        # Computational eta is the native row index times grid spacing, NOT z-z_surface.
        spacing=case['meta'].get('spacing_m')
        if spacing is None:raise ValueError('eta coordinates require metadata spacing_m')
        z=np.broadcast_to(np.arange(int(values['nz']))[::sz,None]*spacing,x.shape).copy()
    elif coordinate=='surface_relative':z-=z[0:1,:]
    elif coordinate!='physical':raise ValueError('depth_coordinate: physical/surface_relative/eta')
    a=values['values'];crop=options.get('crop_m')
    if crop is not None:
        xmin,xmax,zmin,zmax=crop
        valid=(x>=xmin)&(x<=xmax)&(z>=zmin)&(z<=zmax)
        rows=np.flatnonzero(valid.any(axis=1));cols=np.flatnonzero(valid.any(axis=0))
        if not len(rows) or not len(cols):raise ValueError(f'Empty crop for {case["case"]["id"]}')
        rs=slice(rows[0],rows[-1]+1);cs=slice(cols[0],cols[-1]+1)
        x=x[rs,cs];z=z[rs,cs];a=a[rs,cs]
    return x,z,a,values

def spatial(ax,x,z,a,title,cmap,norm=None,limits=None,options=None):
    options=options or {}
    im=ax.pcolormesh(x,z,a,shading='auto',cmap=cmap,norm=norm,
                     vmin=limits[0] if limits else None,vmax=limits[1] if limits else None,rasterized=True)
    ax.set(title=title,xlabel='相对震源 X / m' if options.get('relative_x') else 'X / m',
           ylabel={'eta':'曲网格坐标 η / m','surface_relative':'地表下物理深度 / m'}.get(options.get('depth_coordinate'),'基准深度 Z / m'))
    ax.invert_yaxis();ax.set_aspect('equal' if options.get('equal_aspect',True) else 'auto')
    if options.get('crop_m'):
        x0,x1,z0,z1=options['crop_m'];ax.set(xlim=(x0,x1),ylim=(z1,z0))
    reference_lines(ax,options.get('reference_depths_m',[]),options)
    return im

def symlog(vmax,fraction):
    vmax=max(float(vmax),np.finfo(float).tiny)
    if not 0<fraction<1:raise ValueError('linthresh_fraction must be in (0,1)')
    return SymLogNorm(linthresh=vmax*fraction,vmin=-vmax,vmax=vmax,base=10)

def wave_colorbar(fig,im,axes,options=None):
    from matplotlib.ticker import FuncFormatter
    options=options or {};m=im.norm.vmax;core=im.norm.linthresh
    ticks=sorted(set([-m,-.1*m,-core,0,core,.1*m,m]));scale,unit=velocity_scale(options.get('display_velocity_unit','m/s'))
    cb=fig.colorbar(im,ax=axes,label=f'速度分量 / ({unit})，对称对数；线性核±{core*scale:g}',shrink=.75,ticks=ticks,
        location=options.get('colorbar_location','bottom'),aspect=75,pad=.06,extend='both')
    cb.formatter=FuncFormatter(lambda v,pos:f'{v*scale:g}');cb.update_ticks();return cb

def snapshot_rows(case,targets=None):
    rows=table(case['snapshot_index']);found=[]
    if not rows:raise ValueError(f'Empty snapshot index: {case["snapshot_index"]}')
    for target in targets if targets is not None else [float(r['target_time_s']) for r in rows]:
        row=min(rows,key=lambda r:abs(float(r['target_time_s'])-target))
        if abs(float(row['target_time_s'])-target)>1e-6:raise ValueError(f'Snapshot target {target}s absent in {case["snapshot_index"]}')
        p=Path(row['file']);p=p if p.is_absolute() else case['snapshot_dir']/p
        found.append(dict(row,path=p))
    return found

def metric(a,b):
    if a.shape!=b.shape:raise ValueError('Record shapes differ; explicit time/geometry alignment required')
    denom=np.linalg.norm(b)
    if denom==0:raise ValueError('Zero reference record norm')
    return dict(relative_l2_pct=float(100*np.linalg.norm(a-b)/denom),
                correlation=float(np.corrcoef(a.ravel(),b.ravel())[0,1]),rms_ratio=float(np.linalg.norm(a)/denom))

def gallery(out,pattern='*.png',limit=4):
    from IPython.display import display,Image
    for p in sorted((Path(out)/'figures').glob(pattern))[:limit]:display(Image(filename=str(p),width=1000))
