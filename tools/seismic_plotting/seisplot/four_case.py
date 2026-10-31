"""Terrain × near-surface structure comparisons, shared by every source frequency."""
import json
import numpy as np
from scipy import signal,optimize
from matplotlib import pyplot as plt
from .publication import resolve_style,ordered_components,velocity_scale
from .io import read_vts
from .spectra import trace_amplitude_spectra
from .core import (setup,save,load_cases,records,gain,axes_grid,gather_image,grid_data,
                   spatial,symlog,wave_colorbar,snapshot_rows,write_rows,PALETTE)

class FourCaseStudy:
    def __init__(self,cfg):
        cfg=resolve_style(cfg);self.cfg=cfg;self.out=setup(cfg)
        self.datasets=[dict(config=d,cases=load_cases(d),label=d.get('label',f'dataset_{i+1}')) for i,d in enumerate(cfg['datasets'])]
        if not self.datasets:raise ValueError('Configure at least one input dataset')
        if len({d['label'] for d in self.datasets})!=len(self.datasets):raise ValueError('Dataset labels must be unique')
        for d in self.datasets:
            ids=[c['case']['id'] for c in d['cases']]
            if not ids or len(set(ids))!=len(ids):raise ValueError('Each dataset needs nonempty, unique case ids')
        self._records={};self.metrics=[]

    def get(self,d,c,component):
        key=(d['label'],c['case']['id'],component)
        if key not in self._records:self._records[key]=records(c,component)
        return self._records[key]

    def mask(self,g):
        skip=int(self.cfg.get('analysis',{}).get('exclude_edge_traces',0));mask=np.ones(len(g.x),bool)
        if skip:
            if skip*2>=len(mask):raise ValueError('exclude_edge_traces removes every trace')
            mask[:skip]=mask[-skip:]=False
        return mask

    def inspect(self):
        rows=[]
        for d in self.datasets:
            for c in d['cases']:
                for comp in c['components']:
                    g=self.get(d,c,comp)
                    rows.append(dict(dataset=d['label'],case=c['case']['id'],component=comp,
                        traces=len(g.x),samples=g.values.shape[1],dt_s=g.dt,duration_s=g.t[-1],input_file=str(c['components'][comp])))
        write_rows(self.out/'data/input_inventory.csv',rows);return rows

    def plot_records(self):
        cfg=self.cfg;opt=cfg.get('records',{});files=[];policies=[]
        layouts=opt.get('layouts',['component_matrices'])
        if set(layouts)-{'component_matrices','component_pairs'}:raise ValueError('Unknown records layout')
        for d in self.datasets:
            components=ordered_components(set.intersection(*(set(c['components']) for c in d['cases'])),opt.get('component_order',['Vz','Vx']))
            if 'component_matrices' in layouts:
                for comp in components:
                    gs=[self.get(d,c,comp) for c in d['cases']]
                    raw_limit=opt.get('raw_limit_m_s')
                    if raw_limit is None:raw_limit=max(float(np.quantile(abs(np.concatenate([g.values[self.mask(g)].ravel() for g in gs])),opt.get('clip_quantile',.997))),1e-30)
                    for mode in opt.get('modes',['raw','rms','agc']):
                        arrays=[]
                        for g in gs:
                            rms=np.sqrt(np.mean(np.trapezoid(g.values[self.mask(g)]**2,g.t,axis=-1))/max(g.t[-1],g.dt))
                            arrays.append(gain(g.values,g.dt,mode,opt.get('agc_window_s',.3),opt.get('agc_floor',.01),opt.get('agc_edge','truncated'),opt.get('agc_odd_window',False),rms))
                        limit=raw_limit if mode=='raw' else opt.get('agc_clip',3.) if mode=='agc' else max(float(np.quantile(abs(np.concatenate([a.ravel() for a in arrays])),.997)),1e-30)
                        fig,axs=axes_grid(len(gs),2)
                        for ax,c,g,a in zip(axs,d['cases'],gs,arrays):im=gather_image(ax,g,a,c['case'].get('label',c['case']['id']),cfg,limit,mode=mode)
                        scale,unit=velocity_scale(opt.get('velocity_unit','m/s')) if mode=='raw' else (1.,'RMS')
                        fig.colorbar(im,ax=axs,location=opt.get('colorbar_location','bottom'),shrink=.8,aspect=70,pad=.06,label=f'{comp} / ({unit})'+('，显示增益，非原始振幅' if mode!='raw' else '，无AGC'))
                        fig.suptitle(f'{d["label"]}  {comp}  {mode}，共同显示限幅±{limit*scale:g}')
                        files+=save(fig,self.out,f'{d["label"]}_gathers_{comp}_{mode}',cfg)
                        if opt.get('individual_panels',False):
                            # Reuse the exact comparison scale and already computed display array.
                            # A panel is a separate image, not a re-normalized observation.
                            panel_cfg=dict(cfg)
                            if cfg.get('journal_layout'):
                                panel_cfg.pop('journal_options',None)
                                panel_cfg['journal_layout']='ogp_single'
                                panel_cfg=resolve_style(panel_cfg)
                            for c,g,a in zip(d['cases'],gs,arrays):
                                panel,ax=plt.subplots(figsize=(6.2,3.5),layout='constrained')
                                img=gather_image(ax,g,a,c['case'].get('label',c['case']['id']),cfg,limit,mode=mode)
                                panel.colorbar(img,ax=ax,location=opt.get('colorbar_location','bottom'),pad=.18,
                                    label=f'{comp} / ({unit})'+('，显示增益，非原始振幅' if mode!='raw' else '，无AGC'))
                                files+=save(panel,self.out,f'{d["label"]}_gathers_{comp}_{mode}_{c["case"]["id"]}',panel_cfg)
                        policies.append(dict(dataset=d['label'],component=comp,layout='component_matrices',mode=mode,limit=limit*scale,unit=unit))
            if 'component_pairs' in layouts:
                if components[:2]!=['Vz','Vx']:raise ValueError('component_pairs requires both Vz and Vx')
                by_id={c['case']['id']:c for c in d['cases']}
                for group in opt.get('pair_groups',[]):
                    cases=[by_id[cid] for cid in group['cases']];limit=group['raw_limit_m_s']
                    if not np.isfinite(limit) or limit<=0:raise ValueError('Pair group raw_limit_m_s must be positive')
                    fig,axs=plt.subplots(len(cases),2,figsize=(12,3.25*len(cases)),layout='constrained',squeeze=False)
                    for row,c in enumerate(cases):
                        for col,comp in enumerate(['Vz','Vx']):
                            g=self.get(d,c,comp);im=gather_image(axs[row,col],g,g.values,f'{c["case"]["id"]}  {comp}  {d["label"]}',cfg,limit,relative=True,mode='raw')
                    _,unit=velocity_scale(opt.get('velocity_unit','m/s'))
                    fig.colorbar(im,ax=list(axs.flat),location='bottom',aspect=75,shrink=.8,pad=.06,label=f'原始速度 / ({unit})，同组共同色限，无AGC；左Vz、右Vx')
                    files+=save(fig,self.out,f'{d["label"]}_gathers_{group["id"]}_Vz_Vx_raw',cfg)
                    policies.append(dict(dataset=d['label'],component='Vz,Vx',layout='component_pairs',mode='raw',limit=limit,unit='m/s',group=group['id']))
        write_rows(self.out/'data/record_display_policy.csv',[{k:r.get(k,'') for k in ['dataset','component','layout','mode','limit','unit','group']} for r in policies])
        return files

    def plot_trace_spectra(self):
        """Rainbow trace FFT amplitudes with a black arithmetic mean spectrum."""
        from matplotlib.colors import to_hex
        opt=self.cfg.get('trace_spectra',{});files=[];panels=[];policy=[]
        # Legacy max_hz remains an explicit display override after profile merging.
        max_hz=float(opt.get('max_hz',opt.get('display_max_hz',100)));window=opt.get('window','hann')
        if not np.isfinite(max_hz) or max_hz<=0:raise ValueError('trace_spectra.display_max_hz must be positive')
        for d in self.datasets:
            comps=ordered_components(set.intersection(*(set(c['components']) for c in d['cases'])),['Vz','Vx'])
            for comp in comps:
                fig,axs=axes_grid(len(d['cases']),2,height=3.65)
                for ax,c in zip(axs,d['cases']):
                    g=self.get(d,c,comp);full_f,full_amplitude,full_mean=trace_amplitude_spectra(g.values,g.dt,window)
                    use=full_f<=max_hz
                    if use.sum()<2:raise ValueError('Spectrum display band contains fewer than two bins')
                    f,amplitude,mean=full_f[use],full_amplitude[:,use],full_mean[use]
                    colors=[to_hex(plt.get_cmap('plasma')(v)) for v in np.linspace(.03,.98,len(amplitude))]
                    peak=float(amplitude.max());upper=max(peak*1.08,1e-15)
                    magnitude=10**np.floor(np.log10(upper));step=magnitude/2
                    upper=float(np.ceil(upper/step)*step)
                    for curve,color in zip(amplitude,colors):ax.plot(f,curve,color=color,lw=.5,alpha=.75)
                    ax.plot(f,mean,color='#263430',lw=2,label='平均频谱')
                    ax.set(xlabel='频率 / Hz',ylabel='振幅 / (m/s)',title=c['case'].get('label',c['case']['id']),
                           xlim=(0,max_hz),ylim=(0,upper),facecolor='#fdfaf4')
                    ax.ticklabel_format(axis='y',style='sci',scilimits=(0,0),useMathText=True)
                    ax.grid(alpha=.17);ax.legend(fontsize=11,loc='upper right')
                    stem=f'{d["label"]}_{c["case"]["id"]}_{comp}'
                    write_rows(self.out/'data'/f'{stem}_trace_spectra.csv',[
                        dict(frequency_hz=float(ff),mean_amplitude_m_s=float(full_mean[j]),
                             **{f'trace_{i+1:03d}_amplitude_m_s':float(full_amplitude[i,j]) for i in range(len(full_amplitude))})
                        for j,ff in enumerate(full_f)])
                    write_rows(self.out/'data'/f'{stem}_trace_geometry.csv',[
                        dict(trace=i+1,x_m=float(g.x[i]),offset_m=float(g.offset[i])) for i in range(len(amplitude))])
                    row=dict(dataset=d['label'],case=c['case']['id'],component=comp,traces=len(amplitude),samples=g.values.shape[1],
                        dt_s=g.dt,window=window,detrend='whole-trace mean',normalization='one-sided FFT amplitude / window sum',
                        frequency_step_hz=float(full_f[1]-full_f[0]),display_max_hz=max_hz,data_max_hz=float(full_f[-1]),input_file=str(c['components'][comp]),
                        excluded_traces=0,mean='arithmetic mean of linear trace amplitudes',y_scale='linear, independently fitted per model')
                    policy.append(row)
                    panels.append(dict(**row,label=c['case'].get('label',c['case']['id']),frequency_hz=f.tolist(),
                        trace_amplitude_m_s=amplitude.tolist(),mean_amplitude_m_s=mean.tolist(),trace_colors=colors,ylim=[0,upper]))
                fig.suptitle(f'{d["label"]}  {comp}  多道振幅频谱与平均谱',fontsize=self.cfg.get('title_size',16))
                fig.supxlabel('彩色：各道振幅谱；黑色：平均频谱。原始全时窗，无AGC或逐道归一化；各模型纵轴按实际幅度适配',fontsize=11)
                files+=save(fig,self.out,f'{d["label"]}_trace_spectra_{comp}',self.cfg)
        write_rows(self.out/'data/trace_spectra_policy.csv',policy)
        (self.out/'data/trace_spectra_panels.json').write_text(json.dumps(panels,ensure_ascii=False),encoding='utf-8')
        return files

    def plot_models(self,shallow=False):
        opt=dict(self.cfg.get('models',{}));files=[]
        if shallow:opt.update(self.cfg.get('shallow_models',{}))
        field=opt.get('field','Vp');limits=opt.get('limits_m_s')
        for d in self.datasets:
            data=[grid_data(c,field,self.out,opt) for c in d['cases']]
            scale=limits or [min(float(a[3]['full_min']) for a in data),max(float(a[3]['full_max']) for a in data)]
            fig,axs=axes_grid(len(data),2)
            for ax,c,(x,z,a,_) in zip(axs,d['cases'],data):
                im=spatial(ax,x,z,a,c['case'].get('label',c['case']['id']),opt.get('cmap','turbo'),limits=scale,options=opt)
                source=c['meta'].get('source_x_m')
                if source is not None:
                    xx=0 if opt.get('relative_x') else source
                    zz=c['meta'].get('source_depth_eta_m',c['meta'].get('source_depth_m',0)) if opt.get('depth_coordinate')=='eta' else c['meta'].get('source_z_m',c['meta'].get('source_depth_m',0))
                    ax.plot(xx,zz,'*',color='#c00000',ms=9)
            fig.colorbar(im,ax=axs,label=f'{field} / (m/s)')
            files+=save(fig,self.out,f'{d["label"]}_models_{field}'+('_shallow' if shallow else ''),self.cfg)
        return files

    def plot_wavefields(self,tag=''):
        opt=self.cfg.get('snapshots',{});files=[];policy=[]
        if not opt.get('enabled',True):return []
        scope=opt.get('scale_scope','global')
        for d in self.datasets:
            groups=[snapshot_rows(c,opt.get('times_s')) for c in d['cases']]
            if not groups or not groups[0]:raise ValueError('No wavefield frames selected')
            if len({len(rows) for rows in groups})!=1:raise ValueError('Snapshot lists differ')
            for comp in opt.get('components',['Vz']):
                limit=opt.get('vmax_m_s')
                if limit is None and scope=='global':
                    # Scan/cache extrema, not all frames plus all coordinate arrays in RAM.
                    limit=0.
                    for rows in groups:
                        for row in rows:
                            data=read_vts(row['path'],comp,opt.get('stride_x',1),opt.get('stride_z',1),self.out/'cache')
                            limit=max(limit,abs(float(data['full_min'])),abs(float(data['full_max'])))
                            del data
                for i in range(len(groups[0])):
                    frame=[(c,rows[i],grid_data(c,comp,self.out,opt,rows[i]['path'])) for c,rows in zip(d['cases'],groups)]
                    targets={float(row['target_time_s']) for _,row,_ in frame}
                    if len(targets)!=1:raise ValueError('Snapshot target times differ')
                    target=next(iter(targets));frame_limit=limit if limit is not None else max(float(abs(v[2]).max()) for _,_,v in frame)
                    frame_limit=max(float(frame_limit),np.finfo(float).tiny*100)
                    fraction=opt.get('linthresh_m_s',frame_limit*opt.get('linthresh_fraction',.01))/max(frame_limit,np.finfo(float).tiny)
                    norm=symlog(frame_limit,fraction);fig,axs=axes_grid(len(frame),2)
                    for ax,(c,row,(x,z,a,_)) in zip(axs,frame):im=spatial(ax,x,z,a,c['case'].get('label',c['case']['id'])+f" t={float(row['actual_time_s']):.6f}s",opt.get('cmap','coolwarm'),norm=norm,options=opt)
                    wave_colorbar(fig,im,axs,opt);files+=save(fig,self.out,f'{d["label"]}_{comp}_compare_t{target:.2f}_{opt.get("cmap","coolwarm")}{tag}',self.cfg)
                    if opt.get('individual',False):
                        for c,row,(x,z,a,_) in frame:
                            fig,ax=plt.subplots(figsize=(12,5),layout='constrained')
                            im=spatial(ax,x,z,a,c['case'].get('label',c['case']['id'])+f" t={float(row['actual_time_s']):.6f}s",opt.get('cmap','coolwarm'),norm=norm,options=opt)
                            wave_colorbar(fig,im,[ax],opt);files+=save(fig,self.out,f'{d["label"]}_{c["case"]["id"]}_{comp}_t{target:.2f}_{opt.get("cmap","coolwarm")}{tag}',self.cfg)
                    policy.append(dict(dataset=d['label'],component=comp,target_time_s=target,actual_times_s=';'.join(row['actual_time_s'] for _,row,_ in frame),vmax_m_s=frame_limit,linthresh_m_s=norm.linthresh,scale_scope='fixed' if opt.get('vmax_m_s') is not None else scope,colormap=opt.get('cmap','coolwarm'),reference_depths_m=';'.join(map(str,opt.get('reference_depths_m',[])))))
                    del frame
        write_rows(self.out/'data/wavefield_display_policy.csv',policy);return files

    def plot_metrics(self):
        files=[];metrics=[];opt=self.cfg.get('analysis',{});spectra=[]
        for d in self.datasets:
            fig,axs=plt.subplots(1,2,figsize=(13,4.5),layout='constrained')
            figspec,spax=plt.subplots(1,2,figsize=(13,4.5),layout='constrained')
            figsym,symax=plt.subplots(1,2,figsize=(13,4.5),layout='constrained')
            for i,c in enumerate(d['cases']):
                comps=list(c['components']);gs=[self.get(d,c,p) for p in comps];g=gs[0];mask=self.mask(g)
                if any(not np.array_equal(a.x,g.x) or a.dt!=g.dt or a.values.shape!=g.values.shape for a in gs):raise ValueError('Component time/geometry mismatch')
                power=sum(a.values*a.values for a in gs);T=max(g.t[-1],g.dt)
                E=float(np.mean(np.trapezoid(power[mask],g.t,axis=-1)));near=mask&(abs(g.offset)<=opt.get('near_offset_m',1000))
                late=g.t>=opt.get('late_start_s',g.t[-1]/2)
                frac=float(np.sum(np.trapezoid(power[near][:,late],g.t[late],axis=-1))/np.sum(np.trapezoid(power[near],g.t,axis=-1))) if near.any() and late.sum()>1 else float('nan')
                label=c['case'].get('label',c['case']['id']);color=PALETTE[i%len(PALETTE)]
                row=dict(dataset=d['label'],case=c['case']['id'],vector_rms_m_s=float(np.sqrt(E/T)),squared_velocity_integral_m2_s=E,near_late_fraction=frac,frequency_hz=c['meta'].get('frequency_hz',''))
                for comp,a in zip(comps,gs):row[comp+'_rms_m_s']=float(np.sqrt(np.mean(np.trapezoid(a.values[mask]**2,g.t,axis=-1))/T))
                metrics.append(row)
                for ax,use in zip(axs,[mask,near]):
                    if use.any():ax.semilogy(g.t,np.maximum(signal.convolve(power[use].mean(0),np.ones(max(1,round(.05/g.dt)))/max(1,round(.05/g.dt)),mode='same'),1e-30),color=color,label=label)
                nperseg=min(opt.get('welch_samples',2048),len(g.t));psd=None
                for a in gs:
                    f,p=signal.welch(a.values[mask],fs=1/g.dt,nperseg=nperseg,axis=-1);psd=p.mean(0) if psd is None else psd+p.mean(0)
                use=f<=opt.get('spectrum_max_hz',60);area=np.trapezoid(psd[use],f[use])
                spax[0].semilogy(f[use],np.maximum(psd[use],1e-30),c=color,label=label)
                spax[1].plot(f[use],psd[use]/max(area,1e-30),c=color,label=label)
                row['spectral_centroid_hz']=float(np.trapezoid(f[use]*psd[use],f[use])/max(area,1e-30))
                write_rows(self.out/'data'/f'{d["label"]}_{c["case"]["id"]}_spectrum.csv',[dict(frequency_hz=float(ff),psd_m2_s2_hz=float(pp)) for ff,pp in zip(f,psd)])
                if 'Vz' in comps:
                    gz=gs[comps.index('Vz')];right=np.flatnonzero(mask&(g.offset>=opt.get('mirror_min_m',400))&(g.offset<=opt.get('mirror_max_m',3000)))
                    pairs=[]
                    for r in right:
                        l=int(np.argmin(abs(g.offset+g.offset[r])))
                        if mask[l] and abs(g.offset[l]+g.offset[r])<max(1e-6,np.ptp(g.x)*1e-6):pairs.append((r,l))
                    if pairs:
                        def cosine(a,b):return float(np.sum(a*b)/max(np.linalg.norm(a)*np.linalg.norm(b),1e-30))
                        vals=[cosine(gz.values[r],gz.values[l]) for r,l in pairs]
                        symax[0].plot([g.offset[r] for r,l in pairs],vals,color=color,label=label)
                        right,left=map(list,zip(*pairs))
                        row['mirror_vz_correlation']=cosine(gz.values[right],gz.values[left])
                        row['mirror_vz_mean_trace_correlation']=float(np.mean(vals))
                        if 'Vx' in comps:
                            gx=gs[comps.index('Vx')];row['mirror_vx_correlation']=cosine(gx.values[right],-gx.values[left])
                bins=np.linspace(0,max(abs(g.offset)),opt.get('offset_bins',12)+1);centers=(bins[:-1]+bins[1:])/2
                vals=[np.sqrt(power[mask&(abs(g.offset)>=lo)&(abs(g.offset)<hi)].mean()) if (mask&(abs(g.offset)>=lo)&(abs(g.offset)<hi)).any() else np.nan for lo,hi in zip(bins[:-1],bins[1:])]
                symax[1].semilogy(centers,vals,color=color,label=label)
            for ax,title in zip(axs,['全部分析道','近炮点分析道']):ax.set(xlabel='时间 / s',ylabel='平均平方速度 / (m²/s²)',title=title);ax.legend(fontsize=9)
            spax[0].set(xlabel='频率 / Hz',ylabel='功率谱密度 / (m²/s²/Hz)',title='原始物理幅度')
            spax[1].set(xlabel='频率 / Hz',ylabel='谱面积归一化 / (1/Hz)',title='谱形对比')
            symax[0].set(xlabel='绝对偏移距 / m',ylabel='Vz镜像相关系数',ylim=(-1.05,1.05));symax[1].set(xlabel='绝对偏移距 / m',ylabel='矢量RMS / (m/s)')
            for ax in [*spax,*symax]:
                if ax.lines:ax.legend(fontsize=9)
            files+=save(fig,self.out,d['label']+'_time_response',self.cfg)+save(figspec,self.out,d['label']+'_spectra',self.cfg)+save(figsym,self.out,d['label']+'_symmetry_offset',self.cfg)
        # Fill optional columns consistently for non-symmetric acquisition layouts.
        keys=sorted(set().union(*(r.keys() for r in metrics)))
        write_rows(self.out/'data/record_metrics.csv',[{k:r.get(k,'') for k in keys} for r in metrics]);self.metrics=metrics
        return files

    def plot_frequency_compare(self):
        if not self.metrics:self.plot_metrics()
        fig,axs=plt.subplots(1,2,figsize=(13,5),layout='constrained')
        ids=list(dict.fromkeys(r['case'] for r in self.metrics))
        for i,key in enumerate(ids):
            rows=[r for r in self.metrics if r['case']==key and r['frequency_hz']!='']
            rows.sort(key=lambda r:r['frequency_hz'])
            axs[0].plot([r['frequency_hz'] for r in rows],[r['vector_rms_m_s'] for r in rows],'o-',color=PALETTE[i%len(PALETTE)],label=key)
            axs[1].plot([r['frequency_hz'] for r in rows],[r['near_late_fraction']*100 for r in rows],'o-',color=PALETTE[i%len(PALETTE)],label=key)
        axs[0].set(xlabel='震源主频 / Hz',ylabel='原始矢量RMS / (m/s)');axs[1].set(xlabel='震源主频 / Hz',ylabel='近炮晚时平方速度积分占比 / %')
        for ax in axs:ax.legend()
        return save(fig,self.out,'source_frequency_metrics',self.cfg)

    def plot_terrain_profile(self):
        files=[]
        for d in self.datasets:
            cfg=d['config'];candidate=next((c for c in d['cases'] if c['meta'].get('terrain')),None)
            if candidate is None:continue
            from .core import resolve
            path=resolve(cfg.get('surface_profile_csv','surface_profile.csv'),resolve(cfg['root']))
            datum=candidate['meta'].get('surface_datum_m')
            if path.is_file():
                data=np.atleast_2d(np.loadtxt(path,delimiter=',',skiprows=1));x,y=data[:,0],data[:,1];ylabel='地表高程 / m'
            else:
                g=self.get(d,candidate,next(iter(candidate['components'])));x=g.x;y=datum-g.z if datum is not None else g.z;ylabel='地表高程 / m' if datum is not None else '地表基准深度 / m'
            fig,ax=plt.subplots(figsize=(12,3.5),layout='constrained');ax.plot(x,y,color='#28566a',lw=1.15)
            if datum is not None:
                ax.axhline(datum,color='#888888',lw=1,ls='--')
                ax.plot(candidate['meta']['source_x_m'],datum-candidate['meta']['source_z_m'],'*',color='#c00000',ms=11)
            elif ylabel.startswith('地表基准'):ax.invert_yaxis()
            ax.set(xlabel='水平距离 X / m',ylabel=ylabel,title=d['label']+' 地表剖面')
            files+=save(fig,self.out,d['label']+'_terrain_profile',self.cfg)
        return files

    def plot_reflections(self):
        opt=self.cfg.get('reflection_reference',{})
        if not opt.get('enabled',False):return []
        files=[];qc=[]
        for d in self.datasets:
            fig,axs=axes_grid(len(d['cases']),2);refs=[]
            for ax,c in zip(axs,d['cases']):
                g=self.get(d,c,'Vz');m=c['meta'];low=m.get('low_velocity_layer',False)
                profile=opt['low_velocity_profile'] if low else opt['background_profile']
                source_depth=m.get('source_depth_eta_m',opt.get('source_depth_m',30))
                delay=d['config'].get('source_delay_s',1.5/m['frequency_hz'])
                curves=[]
                rec=self.cfg.get('records',{});rms=np.sqrt(np.mean(np.trapezoid(g.values[self.mask(g)]**2,g.t,axis=-1))/max(g.t[-1],g.dt))
                a=gain(g.values,g.dt,'agc',rec.get('agc_window_s',.3),rec.get('agc_floor',.01),rec.get('agc_edge','truncated'),rec.get('agc_odd_window',False),rms)
                im=gather_image(ax,g,a,c['case'].get('label',c['case']['id']),self.cfg,rec.get('agc_clip',3),mode='agc')
                for ri,depth in enumerate(opt['depths_m']):
                    base=layered_reflection(profile['boundaries_m'],profile['vp_m_s'],source_depth,depth,g.offset,delay)
                    if m.get('terrain',False):
                        diff=np.hypot(g.offset,2*depth-m['source_z_m']-g.z)-np.hypot(g.offset,2*depth-source_depth)
                        base=base+diff/opt.get('geometry_reference_vp_m_s',3000)
                    curves.append(base);display_scale=.001 if self.cfg.get('records',{}).get('distance_unit','m')=='km' else 1.;ax.plot(g.x*display_scale,base,color=['#e31a1c','#ff8c00'][ri%2],lw=1.6,ls='--',label=f'R{ri+1} {depth:g}m参考')
                ax.legend(fontsize=9)
                if opt.get('xlim_m'):ax.set_xlim(*(np.array(opt['xlim_m'])*display_scale))
                if opt.get('time_window_s'):ax.set_ylim(*opt['time_window_s'][::-1])
                refs.append((c,g,curves))
                write_rows(self.out/'data'/f'{d["label"]}_{c["case"]["id"]}_reflection_reference.csv',
                           [dict(x_m=float(x),**{f'R{i+1}_s':float(cc[j]) for i,cc in enumerate(curves)}) for j,x in enumerate(g.x)])
            fig.colorbar(im,ax=axs,label='Vz / 局部RMS，参考曲线不代表拾取')
            files+=save(fig,self.out,d['label']+'_reflection_windows_agc',self.cfg)
            flat=next((r for r in refs if not r[0]['meta'].get('terrain') and not r[0]['meta'].get('low_velocity_layer')),None)
            terrain=next((r for r in refs if r[0]['meta'].get('terrain') and not r[0]['meta'].get('low_velocity_layer')),None)
            if flat and terrain:
                _,ga,ca=flat;_,gb,cb=terrain
                if not np.array_equal(ga.x,gb.x):raise ValueError('Reflection alignment requires matching receiver X')
                u=np.arange(-opt.get('correlation_halfwidth_s',.08),opt.get('correlation_halfwidth_s',.08)+ga.dt/2,ga.dt)
                delayms=(cb[0]-ca[0])*1000;before=[];after=[]
                for j in range(len(ga.x)):
                    aa=np.interp(ca[0][j]+u,ga.t,ga.values[j]);bb=np.interp(ca[0][j]+u,gb.t,gb.values[j]);cc=np.interp(cb[0][j]+u,gb.t,gb.values[j])
                    before.append(float(np.corrcoef(aa,bb)[0,1]));after.append(float(np.corrcoef(aa,cc)[0,1]))
                    qc.append(dict(dataset=d['label'],receiver=j,x_m=float(ga.x[j]),reference_delay_ms=float(delayms[j]),raw_correlation=before[-1],aligned_correlation=after[-1]))
                fig,axs=plt.subplots(1,3,figsize=(15,4.5),layout='constrained')
                axs[0].plot(ga.x,delayms,color='#ed7d31');axs[0].set(xlabel='接收点 X / m',ylabel='几何参考延迟 / ms')
                axs[1].plot(ga.x,before,'.',color='#888',label='同绝对时间窗');axs[1].plot(ga.x,after,'.',color='#c00000',label='按几何参考对齐')
                axs[1].set(xlabel='接收点 X / m',ylabel='原始记录相关系数',ylim=(-1.05,1.05));axs[1].legend()
                j=int(np.argmin(abs(ga.offset-opt.get('example_offset_m',1000))))
                for g,curves,color,label in [(ga,ca,'#c00000','平地'),(gb,cb,'#ed7d31','起伏')]:
                    global_rms=float(np.sqrt(np.mean(np.trapezoid(g.values[self.mask(g)]**2,g.t,axis=-1))/max(g.t[-1],g.dt)))
                    agc=gain(g.values[[j]],g.dt,'agc',rec.get('agc_window_s',.3),rec.get('agc_floor',.01),rec.get('agc_edge','truncated'),rec.get('agc_odd_window',False),global_rms)
                    axs[2].plot(u*1000,np.interp(curves[0][j]+u,g.t,agc[0]),color=color,label=label)
                axs[2].set(xlabel='相对R1参考时刻 / ms',ylabel='Vz / 局部RMS',title=f'AGC对齐示例，X={ga.x[j]:g}m');axs[2].legend()
                files+=save(fig,self.out,d['label']+'_reflection_alignment',self.cfg)
        write_rows(self.out/'data/reflection_alignment.csv',qc);return files

def layered_reflection(bounds,velocities,source_depth,reflector,offsets,delay):
    bounds=np.array(bounds);velocities=np.array(velocities)
    if len(bounds)!=len(velocities)+1 or source_depth>=reflector or reflector>bounds[-1]:raise ValueError('Invalid layered reflection reference')
    hh=[];vv=[]
    for start in [source_depth,0.]:
        for a,b,v in zip(bounds[:-1],bounds[1:],velocities):
            thickness=max(0,min(reflector,b)-max(start,a))
            if thickness:hh.append(thickness);vv.append(v)
    h=np.array(hh);v=np.array(vv)
    if not len(h):raise ValueError('Reflector outside reference profile')
    result=[]
    for off in offsets:
        p=0 if off==0 else optimize.brentq(lambda p:np.sum(h*p*v/np.sqrt(1-(p*v)**2))-abs(off),0,(1-1e-12)/v.max(),xtol=1e-15)
        result.append(float(np.sum(h/(v*np.sqrt(1-(p*v)**2)))+delay))
    return np.array(result)
