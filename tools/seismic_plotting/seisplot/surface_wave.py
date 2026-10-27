"""Surface-wave plots, independent phase-coherence maps and record differences."""
import hashlib,json
import numpy as np
from scipy import signal,ndimage
from matplotlib import pyplot as plt
from .four_case import FourCaseStudy
from .core import resolve,save,axes_grid,metric,write_rows,PALETTE
from .io import numeric_csv

class SurfaceWaveStudy(FourCaseStudy):
    def __init__(self,cfg):
        if len(cfg['datasets'])!=1:raise ValueError('SurfaceWaveStudy uses one run batch per configuration')
        super().__init__(cfg);self.d=self.datasets[0];self.dispersion={};self.picks=[]
        self.byid={c['case']['id']:c for c in self.d['cases']}
        self.theory=None
        path=cfg.get('theory_curve_csv')
        if path:self.theory=np.atleast_1d(numeric_csv(resolve(path)))

    def compute_dispersion(self):
        opt=self.cfg['dispersion'];fmin,fmax,df=opt['frequency_hz'];vmin,vmax,dv=opt['velocity_m_s']
        if min(fmin,df,vmin,dv)<=0 or fmax<=fmin or vmax<=vmin:raise ValueError('Invalid dispersion search grid')
        freqs=np.arange(fmin,fmax+df/2,df);velocities=np.arange(vmin,vmax+dv/2,dv)
        self.picks=[]
        for cid,c in self.byid.items():
            comp=opt.get('component','Vz');g=self.get(self.d,c,comp)
            if fmax>=.5/g.dt:raise ValueError('Dispersion frequency must be below temporal Nyquist')
            for wi,(lo,hi) in enumerate(opt['offset_windows_m']):
                mask=(g.offset>=lo)&(g.offset<=hi)
                if mask.sum()<3:raise ValueError(f'{cid}: fewer than three receivers in {lo}–{hi}m window')
                stamp=c['components'][comp].stat()
                signature=json.dumps(opt,sort_keys=True)+f'{c["components"][comp]}|{stamp.st_size}|{stamp.st_mtime_ns}|{g.dt}|{g.offset.tolist()}|{wi}'
                digest=hashlib.sha256(signature.encode()).hexdigest()[:24];cache=self.out/'cache'/f'dispersion_{digest}.npz'
                if cache.exists():
                    with np.load(cache) as z:coherence=z['coherence']
                else:
                    a=g.values[mask]*signal.windows.tukey(len(g.t),opt.get('tukey_fraction',.08))[None,:]
                    spec=np.empty((mask.sum(),len(freqs)),complex)
                    for j in range(0,len(freqs),32):spec[:,j:j+32]=a@np.exp(-2j*np.pi*np.outer(g.t,freqs[j:j+32]))
                    unit=spec/np.maximum(abs(spec),1e-30);coherence=np.empty((len(velocities),len(freqs)))
                    for j,f in enumerate(freqs):coherence[:,j]=abs(np.exp(2j*np.pi*f*g.offset[mask][None,:]/velocities[:,None])@unit[:,j])/mask.sum()
                    np.savez_compressed(cache,coherence=coherence,frequencies=freqs,velocities=velocities)
                self.dispersion[(cid,wi)]=dict(freq=freqs,v=velocities,coherence=coherence)
                rows=[]
                for j,f in enumerate(freqs):
                    ix=int(np.argmax(coherence[:,j]));picked=velocities[ix]
                    if 0<ix<len(velocities)-1:
                        aa,bb,cc=coherence[ix-1:ix+2,j];den=aa-2*bb+cc
                        if den<0:picked+=float(np.clip(.5*(aa-cc)/den,-1,1)*dv)
                    row=dict(case=cid,window_index=wi,frequency_hz=float(f),phase_velocity_m_s=float(picked),coherence=float(coherence[ix,j]),at_search_boundary=ix in (0,len(velocities)-1))
                    rows.append(row);self.picks.append(row)
                write_rows(self.out/'data'/f'{cid}_window{wi}_unguided_picks.csv',rows)
        write_rows(self.out/'data/dispersion_unguided_picks.csv',self.picks)
        (self.out/'data/dispersion_method.json').write_text(json.dumps(dict(method='phase-only shift stack; global velocity maximum, optional parabolic interpolation; no theoretical search gate',frequency_spacing_hz=df,
          natural_frequency_spacing_hz=1/(len(g.t)*g.dt),note='Spectral sampling does not increase resolution; global maxima still require independent mode identification.'),indent=2),encoding='utf-8')
        return self.picks

    def _phase_axes(self,ax,cid,wi):
        a=self.dispersion[(cid,wi)];im=ax.pcolormesh(a['freq'],a['v'],a['coherence'],cmap=self.cfg['dispersion'].get('cmap','viridis'),vmin=0,vmax=1,shading='auto',rasterized=True)
        opt=self.cfg['dispersion'];c=self.byid[cid]
        if c['case'].get('uniform_rayleigh_m_s') is not None:
            ax.axhline(c['case']['uniform_rayleigh_m_s'],color='#e31a1c',lw=1.7,label='均匀半空间理论')
        elif self.theory is not None:
            fcol=opt.get('theory_frequency_column','frequency_hz')
            cols=opt.get('theory_mode_columns',['mode0_c_m_s','mode1_c_m_s','mode2_c_m_s'])
            for j,col in enumerate(cols):
                if col in self.theory.dtype.names:ax.plot(self.theory[fcol],self.theory[col],color=['#e31a1c','#ff8c00','#ffffff'][j%3],ls=['-','--',':'][j%3],lw=1.6,label=f'理论模态{j}')
        ax.set(xlabel='频率 / Hz',ylabel='相速度 / (m/s)',title=c['case'].get('label',cid),
               xlim=opt.get('plot_frequency_hz',[a['freq'][0],a['freq'][-1]]),ylim=opt.get('plot_velocity_m_s',[a['v'][0],a['v'][-1]]))
        if ax.lines:ax.legend(fontsize=9)
        return im

    def plot_dispersion(self):
        if not self.dispersion:self.compute_dispersion()
        wi=self.cfg['dispersion'].get('comparison_window_index',0);fig,axs=axes_grid(len(self.byid),3,4)
        for ax,cid in zip(axs,self.byid):im=self._phase_axes(ax,cid,wi)
        fig.colorbar(im,ax=axs,label='相位叠加相干度',shrink=.75)
        return save(fig,self.out,'dispersion_models_compare',self.cfg)

    def plot_offset_windows(self):
        if not self.dispersion:self.compute_dispersion()
        cid=self.cfg['dispersion'].get('anchor_case',next(iter(self.byid)));windows=self.cfg['dispersion']['offset_windows_m']
        fig,axs=axes_grid(len(windows),3,4)
        for wi,(ax,(lo,hi)) in enumerate(zip(axs,windows)):
            im=self._phase_axes(ax,cid,wi);ax.set_title(f'{cid}  偏移距{lo:g}–{hi:g}m')
        fig.colorbar(im,ax=axs,label='相位相干度',shrink=.75)
        return save(fig,self.out,'dispersion_offset_windows',self.cfg)

    def plot_phase_curves(self):
        if not self.picks:self.compute_dispersion()
        wi=self.cfg['dispersion'].get('comparison_window_index',0)
        fig,ax=plt.subplots(figsize=(12,5),layout='constrained')
        for i,(cid,c) in enumerate(self.byid.items()):
            rows=[r for r in self.picks if r['case']==cid and r['window_index']==wi]
            ax.plot([r['frequency_hz'] for r in rows],[r['phase_velocity_m_s'] for r in rows],color=PALETTE[i%len(PALETTE)],label=c['case'].get('label',cid))
        opt=self.cfg['dispersion'];ax.set(xlabel='频率 / Hz',ylabel='全局峰值相速度 / (m/s)',xlim=opt.get('plot_frequency_hz'),ylim=opt.get('plot_velocity_m_s'))
        ax.legend(fontsize=10);return save(fig,self.out,'phase_velocity_unguided',self.cfg)

    def plot_record_differences(self):
        opt=self.cfg.get('differences',{});pairs=opt.get('pairs',[])
        if not pairs:return []
        rows=[];fig,axs=plt.subplots(2,1,figsize=(12,7),layout='constrained')
        for i,(cid,ref) in enumerate(pairs):
            c=self.byid[cid];b=self.byid[ref];components=set(c['components'])&set(b['components'])
            for comp in sorted(components):
                ga=self.get(self.d,c,comp);gb=self.get(self.d,b,comp)
                if ga.dt!=gb.dt or ga.values.shape!=gb.values.shape or not np.allclose(ga.offset,gb.offset):raise ValueError(f'{cid}/{ref}: record dt/size/offset differs')
                signals={'raw':(ga.values,gb.values)}
                if opt.get('bandpass_hz'):
                    lo,hi=opt['bandpass_hz']
                    if not 0<lo<hi<.5/ga.dt:raise ValueError('Invalid difference bandpass')
                    sos=signal.butter(4,[lo,hi],fs=1/ga.dt,btype='bandpass',output='sos')
                    signals['bandpass']=(signal.sosfiltfilt(sos,ga.values,axis=-1),signal.sosfiltfilt(sos,gb.values,axis=-1))
                for band,(a,bv) in signals.items():
                    for window in opt.get('time_windows_s',[[0,float(ga.t[-1])]]):
                        mask=(ga.t>=window[0])&(ga.t<=window[1])
                        if mask.sum()<2:raise ValueError('Time comparison window has fewer than two samples')
                        rows.append(dict(case=cid,reference=ref,component=comp,band=band,time_start_s=window[0],time_end_s=window[1],**metric(a[:,mask],bv[:,mask])))
                if comp==opt.get('display_component','Vz'):
                    diff=ga.values-gb.values;color=PALETTE[i%len(PALETTE)]
                    e=100*np.linalg.norm(diff,axis=0)/max(np.linalg.norm(gb.values),1e-30)
                    axs[0].semilogy(ga.t,np.maximum(ndimage.gaussian_filter1d(e,opt.get('smoothing_s',.005)/ga.dt),1e-12),color=color,label=f'{cid} / {ref}')
                    per=100*np.linalg.norm(diff,axis=1)/np.maximum(np.linalg.norm(gb.values,axis=1),1e-30)
                    axs[1].semilogy(ga.offset,per,color=color,label=f'{cid} / {ref}')
        axs[0].set(xlabel='时间 / s',ylabel='瞬时残差 / 参考总范数（%）');axs[1].set(xlabel='偏移距 / m',ylabel='逐道相对L2差异 / %')
        axs[0].legend(ncol=2,fontsize=10);write_rows(self.out/'data/record_difference_metrics.csv',rows)
        return save(fig,self.out,'boundary_grid_residuals',self.cfg)

    def plot_trace_overlay(self):
        opt=self.cfg.get('trace_overlay',{});targets=opt.get('offsets_m',[40,100,180,260]);comp=opt.get('component','Vz')
        fig,axs=axes_grid(len(targets),2,3)
        for ax,off in zip(axs,targets):
            for i,(cid,c) in enumerate(self.byid.items()):
                if opt.get('cases') and cid not in opt['cases']:continue
                g=self.get(self.d,c,comp);j=int(np.argmin(abs(g.offset-off)))
                ax.plot(g.t,g.values[j],color=PALETTE[i%len(PALETTE)],lw=1.1,label=f'{cid}（{g.offset[j]:g}m）')
            ax.set(xlabel='时间 / s',ylabel=f'{comp} / (m/s)',title=f'目标偏移距{off:g}m')
        axs[0].legend(fontsize=9);return save(fig,self.out,'raw_trace_overlay',self.cfg)
