"""Audit an existing loess pilot and make readable diagnostic figures; no simulations."""
from pathlib import Path
import argparse,csv,hashlib,json,re,struct,sys
sys.dont_write_bytecode=True
import numpy as np
from scipy.ndimage import uniform_filter1d
from scipy.signal import butter,sosfiltfilt,correlate,correlation_lags
from scipy.integrate import cumulative_trapezoid
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import SymLogNorm,Normalize
from matplotlib.ticker import ScalarFormatter
REPO=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(REPO/'tools/seismic_plotting'))
from seisplot.io import read_segy,read_vts,table,vts_shape

CASES=['F_E_L','F_Q25_L','H20_E_L','H20_Q25_L']
LABELS={'F_E_L':'平地 · 弹性','F_Q25_L':'平地 · GSLS','H20_E_L':'沟谷 · 弹性','H20_Q25_L':'沟谷 · GSLS'}
RED='#ba2940';ORANGE='#e68a2e';PURPLE='#76529a'

def load(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def dump(path,obj):Path(path).write_text(json.dumps(obj,ensure_ascii=False,indent=2),encoding='utf-8')
def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda:f.read(8*1024**2),b''):h.update(b)
    return h.hexdigest()
def csvout(path,rows):
    if not rows:return
    with Path(path).open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,list(rows[0]));w.writeheader();w.writerows(rows)
def normerror(a,b):return float(np.linalg.norm(a-b)/max(np.linalg.norm(b),1e-100))

class Review:
    def __init__(self,root,out,dpi,hashes):
        self.root=root;self.out=out;self.dpi=dpi;self.hashes=hashes;self.fig=out/'figures';self.data=out/'data';self.cache=out/'cache'
        for p in [out,self.fig,self.data,self.cache]:p.mkdir(parents=True,exist_ok=True)
        self.catalog=[];self.gathers={};self.models={};self.plotstats=[]
        plt.rcParams.update({'font.family':['Microsoft YaHei','DejaVu Sans'],'font.size':12,'axes.titlesize':14,
                             'axes.labelsize':12,'xtick.labelsize':11,'ytick.labelsize':11,'legend.fontsize':11,
                             'axes.unicode_minus':False,'axes.linewidth':.8,'pdf.fonttype':42,'savefig.facecolor':'white'})
    def save(self,fig,name,title,note):
        fig.savefig(self.fig/(name+'.png'),dpi=self.dpi,facecolor='white')
        fig.savefig(self.fig/(name+'.pdf'),dpi=self.dpi,facecolor='white')
        plt.close(fig);self.catalog.append(dict(file=name+'.png',pdf=name+'.pdf',title=title,note=note))
        print('FIGURE',name,flush=True)
    def get(self,case,component='Vz',fine=True):
        key=(case,component,fine)
        if key not in self.gathers:
            folder=self.root/('qualification' if fine else 'pilot')/(f'REF_{case}_h025' if fine else case)
            self.gathers[key]=read_segy(folder/'records'/f'{component}.sgy',folder/'geometry/receivers.csv',source_x_m=150)
        return self.gathers[key]
    def audit(self):
        plan=load(self.root/'plan.json');rows=[];errors=[]
        for entry in plan['cases']:
            folder=Path(entry['output'])
            if not folder.is_dir():
                rows.append(dict(case_id=entry['case_id'],status='NOT_RUN',samples=0,snapshots=0,hashes=0,peak_Vz_m_s=0));continue
            try:
                assert (folder/'COMPLETE.txt').is_file(),'Missing COMPLETE.txt'
                status=load(folder/'status.json');meta=load(folder/'metadata.json')
                assert status['state']=='complete' and status['finite'],'Incomplete/nonfinite final status'
                assert abs(status['time_s']-meta['duration_s'])<1e-9,'Duration mismatch'
                count=round(meta['duration_s']/status['record_dt_s'])+1
                peaks=[]
                for component in ['Vx','Vz']:
                    g=read_segy(folder/'records'/f'{component}.sgy',folder/'geometry/receivers.csv')
                    assert g.values.shape[1]==count and np.isfinite(g.values).all(),'Record length/finite values'
                    peaks.append(float(abs(g.values).max()))
                schedule=table(folder/'snapshots/index.csv');cfg=(folder/'config.cfg').read_text(encoding='utf-8')
                line=next(l for l in cfg.splitlines() if l.startswith('snapshots '));expected=int(line.split()[1])
                assert len(schedule)==expected,'Snapshot count mismatch'
                for snap in schedule:
                    assert vts_shape(folder/'snapshots'/snap['file'])==(meta['nz'],meta['nx']),'Snapshot extent mismatch'
                    assert abs(float(snap['actual_time_s'])-float(snap['target_time_s']))<=status['dt_s']+1e-10,'Snapshot time mismatch'
                n=0
                if self.hashes:
                    expected_hash=load(folder/'sha256.json')
                    for rel,value in expected_hash.items():
                        assert sha(folder/rel)==value,'SHA256 mismatch: '+rel
                        n+=1
                probe=(folder/'probes/fields_f64.bin')
                with probe.open('rb') as f:
                    assert f.read(8)==b'SWP5F64\n','Probe header'
                    nodes=struct.unpack('<I',f.read(4))[0]
                size=probe.stat().st_size
                if nodes:
                    assert (size-12)%(8+40*nodes)==0,'Probe binary truncated'
                    frames=(size-12)//(8+40*nodes)
                    assert frames==len(table(folder/'probes/index.csv')),'Probe index length'
                    assert frames==round(meta['duration_s']/.001)+1,'Probe target coverage'
                rows.append(dict(case_id=entry['case_id'],status='COMPLETE_CHECKED',samples=count,snapshots=len(schedule),hashes=n,peak_Vz_m_s=peaks[1]))
            except Exception as ex:
                errors.append(dict(case_id=entry['case_id'],error=str(ex)))
                rows.append(dict(case_id=entry['case_id'],status='AUDIT_ERROR',samples=0,snapshots=0,hashes=0,peak_Vz_m_s=0))
            print('AUDIT',entry['case_id'],rows[-1]['status'],flush=True)
        self.audit_result=dict(rows=rows,errors=errors,hashes_checked=self.hashes,
                               completed=sum(r['status']=='COMPLETE_CHECKED' for r in rows),not_run=sum(r['status']=='NOT_RUN' for r in rows))
        dump(self.out/'integrity_audit.json',self.audit_result);csvout(self.data/'case_inventory.csv',rows)
        if errors:raise RuntimeError('Input audit errors; see integrity_audit.json')
    def model(self,case):
        if case not in self.models:
            folder=self.root/'qualification'/f'REF_{case}_h025'
            vs=read_vts(folder/'model/model.vts','Vs',cache_dir=self.cache)['values']
            coords=read_vts(folder/'model/model.vts','Coordinates',cache_dir=self.cache)['values']
            self.models[case]=(coords,vs)
        return self.models[case]
    @staticmethod
    def domain(ax,coords,crop=None):
        x,z=coords[:,:,0],coords[:,:,2]
        ax.plot(x[0],z[0],c=ORANGE,lw=1.5,zorder=4)
        ax.set(xlim=(0,800),ylim=(200,-2),xlabel='水平距离 x / m',ylabel='深度 z / m')
        if crop:ax.set(xlim=crop[:2],ylim=(crop[3],crop[2]))
        ax.set_aspect('equal',adjustable='box')
        ax.tick_params(direction='out');return x,z
    def model_figures(self):
        fig,axs=plt.subplots(2,1,figsize=(13.5,8),layout='constrained')
        for ax,case in zip(axs,['F_Q25_L','H20_Q25_L']):
            coords,vs=self.model(case);x,z=self.domain(ax,coords)
            mesh=ax.pcolormesh(x,z,vs,cmap='turbo',vmin=250,vmax=1300,shading='auto',rasterized=True)
            for eta in [10,30,60]:ax.plot(x[round(eta/.25)],z[round(eta/.25)],c='white',lw=.9,alpha=.9)
            ax.scatter([150],[2],marker='*',s=85,c=RED,edgecolors='white',linewidths=.6,zorder=5)
            ax.set_title(('平地' if case.startswith('F_') else '20 m深沟谷')+'：0.25 m加密参考模型，深宽1∶1')
        cb=fig.colorbar(mesh,ax=axs,orientation='horizontal',fraction=.04,pad=.04,aspect=55);cb.set_label('输入参考频率15 Hz处 Vs / (m/s)')
        self.save(fig,'01_models_Vs','模型与层系','turbo；物理深宽1∶1；白线为计算深度10/30/60 m的界面；尚未证明最终收敛。')
        fig,ax=plt.subplots(figsize=(13.5,4.5),layout='constrained');coords,vs=self.model('H20_Q25_L')
        x,z=self.domain(ax,coords,(280,520,-2,85));m=ax.pcolormesh(x,z,vs,cmap='turbo',vmin=250,vmax=1300,shading='auto',rasterized=True)
        for eta in [10,30,60]:ax.plot(x[round(eta/.25)],z[round(eta/.25)],c='white',lw=1.1)
        fig.colorbar(m,ax=ax,pad=.02,label='Vs / (m/s)');ax.set_title('沟谷浅层局部放大：层系随曲网格变化，深宽1∶1')
        self.save(fig,'02_model_shallow_detail','沟谷浅层局部','裁切x=280–520 m、z=−2–85 m；保持物理比例。')
    def record_figures(self):
        for component in ['Vz','Vx']:
            gs=[self.get(c,component) for c in CASES]
            raw_limit=float(np.quantile(np.concatenate([abs(g.values).ravel() for g in gs]),.997))
            rms=[np.sqrt(np.maximum(uniform_filter1d(g.values*g.values,601,axis=1,mode='constant'),0)) for g in gs]
            floor=.01*max(float(a.max()) for a in rms)
            for kind in ['raw','agc']:
                fig,axs=plt.subplots(2,2,figsize=(13.5,10),sharex=True,sharey=True,layout='constrained')
                for i,(ax,case,g) in enumerate(zip(axs.flat,CASES,gs)):
                    a=g.values if kind=='raw' else g.values/np.maximum(rms[i],floor)
                    limit=raw_limit if kind=='raw' else 3.
                    display=a*1e6 if kind=='raw' else a
                    vmax=limit*1e6 if kind=='raw' else limit
                    image=ax.imshow(display.T,extent=(g.x[0],g.x[-1],g.t[-1],0),aspect='auto',interpolation='bilinear',cmap='gray',vmin=-vmax,vmax=vmax)
                    ax.set_title(LABELS[case]);ax.set_xlabel('接收点水平位置 x / m');ax.set_ylabel('时间 t / s');ax.set_ylim(2.4,0)
                    self.plotstats.append(dict(figure=f'{component}_{kind}',case_id=case,scale=limit,clipped_fraction=float(np.mean(abs(a)>limit)),normalization='none' if kind=='raw' else '300ms sliding RMS; common floor',samples=g.values.shape[1]))
                title=f'{component} · '+('原始振幅，共同灰度色限' if kind=='raw' else '300 ms RMS-AGC，仅用于事件识别')
                fig.suptitle(title+'\n0.25 m加密参考；0–2.4 s；最终收敛未确认',fontsize=16)
                cb=fig.colorbar(image,ax=axs,orientation='horizontal',fraction=.035,pad=.025,aspect=60)
                cb.set_label('原始速度 / (μm/s)' if kind=='raw' else 'AGC归一化幅度；不用于幅度或Qs估计')
                self.save(fig,f'03_records_{component}_{kind}',title,'所有模型同一色限；双线性仅作用于显示；原始数据未平滑或修改。')
                # Large individual gathers avoid illegible four-panel pages.
                for i,(case,g) in enumerate(zip(CASES,gs)):
                    fig,ax=plt.subplots(figsize=(10.5,7),layout='constrained')
                    a=g.values if kind=='raw' else g.values/np.maximum(rms[i],floor)
                    multiplier=1e6 if kind=='raw' else 1.;vmax=raw_limit*1e6 if kind=='raw' else 3.
                    im=ax.imshow((a*multiplier).T,extent=(g.x[0],g.x[-1],2.4,0),aspect='auto',interpolation='bilinear',cmap='gray',vmin=-vmax,vmax=vmax)
                    ax.set(xlabel='接收点水平位置 x / m',ylabel='时间 t / s',title=f'{LABELS[case]} | {component} | '+('原始振幅' if kind=='raw' else 'AGC显示'))
                    fig.colorbar(im,ax=ax,pad=.02,label='μm/s' if kind=='raw' else '仅显示的AGC归一化幅度')
                    self.save(fig,f'gather_{case}_{component}_{kind}',f'{LABELS[case]} {component} {kind}','0.25 m参考，0–2.4 s；与四组矩阵保持同一色限。')
        csvout(self.data/'display_scales_and_clipping.csv',self.plotstats)
    def diagnostics(self):
        gate=load(self.root/'analysis/full_domain_gate.json');names=[LABELS[f'{r["terrain"]}_{r["q"]}_L'] for r in gate['grid_comparisons']]
        fig,axs=plt.subplots(1,2,figsize=(13.5,5.5),layout='constrained')
        for ax,key,title in [(axs[0],'raw_record_l2','原始道集相对L2差'),(axs[1],'median_15hz_amplitude_error','15 Hz幅度差中位数')]:
            values=[100*r[key] for r in gate['grid_comparisons']]
            bars=ax.bar(np.arange(4),values,color=[RED,ORANGE,RED,ORANGE],width=.65)
            ax.axhline(5,color=PURPLE,ls='--',lw=1.8,label='原定5%门槛');ax.set_xticks(range(4),names,rotation=15)
            ax.set(ylabel='差异 / %',title=title);ax.set_ylim(0,max(max(values)*1.25,7));ax.grid(axis='y',alpha=.2)
            for bar,value in zip(bars,values):ax.text(bar.get_x()+bar.get_width()/2,value+.15,f'{value:.2f}%',ha='center',fontsize=12)
            ax.legend(loc='upper right')
        fig.suptitle('0.5 m 对 0.25 m：幅度指标较小，但原始波形未通过网格门槛',fontsize=15)
        self.save(fig,'04_grid_acceptance','网格验收结果','严格保留原始5%门槛；不以显示平滑或时间平移改写验收。')
        rows=[];lag_rows=[]
        bands=[(3,8),(8,15),(15,22),(22,30),(8,30),(3,45)]
        for case in CASES:
            base=self.get(case,fine=False);fine=self.get(case);count=fine.values.shape[1]
            selected=(base.x>=230)&(base.x<=580)
            a,b=base.values[selected,:count],fine.values[selected]
            for lo,hi in bands:
                sos=butter(4,[lo,hi],btype='bandpass',fs=1/base.dt,output='sos')
                rows.append(dict(case_id=case,low_hz=lo,high_hz=hi,bandpassed_raw_l2=normerror(sosfiltfilt(sos,a,axis=1),sosfiltfilt(sos,b,axis=1))))
            for x in [250,300,350,400,450,500,550]:
                ix=int(np.argmin(abs(fine.x-x)));ref=fine.values[ix];test=base.values[ix,:count]
                valid=(fine.t>=.15)&(fine.t<=2.15);center=float(fine.t[np.flatnonzero(valid)[np.argmax(abs(ref[valid]))]])
                win=abs(fine.t-center)<=.18;aa=test[win];bb=ref[win]
                cc=correlate(aa,bb,mode='full',method='fft');lags=correlation_lags(len(aa),len(bb))*fine.dt
                available=abs(lags)<=.02;idx=np.flatnonzero(available)[np.argmax(cc[available])];delta=0.
                if 0<idx<len(cc)-1:
                    denominator=cc[idx-1]-2*cc[idx]+cc[idx+1]
                    if denominator!=0:delta=.5*(cc[idx-1]-cc[idx+1])/denominator
                lag=float(lags[idx]+delta*fine.dt)
                aligned=np.interp(fine.t+lag,fine.t,test)
                lag_rows.append(dict(case_id=case,x_m=x,window_center_s=center,coarse_delay_ms=lag*1000,window_raw_l2=normerror(test[win],ref[win]),diagnostic_aligned_l2=normerror(aligned[win],ref[win])))
            if case in ['F_Q25_L','H20_Q25_L']:
                fig,axs=plt.subplots(3,1,figsize=(12,9),layout='constrained')
                for ax,x in zip(axs,[250,400,550]):
                    row=next(r for r in lag_rows if r['case_id']==case and r['x_m']==x);ix=int(np.argmin(abs(fine.x-x)))
                    scale=max(abs(fine.values[ix]).max(),1e-100)
                    ax.plot(fine.t,base.values[ix,:count]/scale,c=ORANGE,lw=1.8,label='0.5 m候选')
                    ax.plot(fine.t,fine.values[ix]/scale,c=RED,lw=1.5,ls='--',label='0.25 m加密参考')
                    ax.set(xlim=(row['window_center_s']-.18,row['window_center_s']+.18),xlabel='实际时间 t / s',ylabel='同一道共同归一化幅度',title=f'x={x} m；诊断互相关时移={row["coarse_delay_ms"]:.2f} ms')
                    ax.grid(alpha=.2);ax.legend(loc='upper right')
                fig.suptitle(LABELS[case]+'：未做时间平移的原始波形局部放大',fontsize=16)
                self.save(fig,'05_trace_zoom_'+case,'网格波形放大 '+LABELS[case],'每道两网格使用相同峰值尺度；保持真实时间轴，没有通过时间对齐改变显示。')
        csvout(self.data/'bandwise_grid_errors.csv',rows);csvout(self.data/'trace_delay_diagnostics.csv',lag_rows)
        fig,ax=plt.subplots(figsize=(12,5),layout='constrained')
        for i,case in enumerate(CASES):
            selected=[r for r in lag_rows if r['case_id']==case]
            ax.plot([r['x_m'] for r in selected],[r['coarse_delay_ms'] for r in selected],color=RED if i<2 else ORANGE,ls='-' if i%2==0 else '--',marker='o',lw=1.9,label=LABELS[case])
        ax.axhline(0,c='#555555',lw=.8);ax.set(xlabel='接收点水平位置 x / m',ylabel='0.5 m相对0.25 m的诊断时移 / ms',title='主波窗互相关时移：用于定位相位/走时差，不用于修订验收');ax.legend(ncols=2);ax.grid(alpha=.2)
        self.save(fig,'06_grid_delay_diagnostics','波形走时诊断','互相关只在主波±0.18 s窗、±20 ms搜索；混合波型下不是严格相速度。')
        self.lags=lag_rows;self.band_errors=rows
        # Explicitly show the O(h) midpoint shift in the node-assigned layer interface.
        fig,axs=plt.subplots(1,2,figsize=(12,5.5),layout='constrained')
        for h,col,ls in [(.5,ORANGE,'-'),(.25,RED,'--')]:
            folder=self.root/('pilot/F_Q25_L' if h==.5 else 'qualification/REF_F_Q25_L_h025')
            vs=read_vts(folder/'model/model.vts','Vs',cache_dir=self.cache)['values'][:,0]
            eta=np.arange(len(vs))*h
            for ax,center in zip(axs,[10,30]):
                ax.step(vs,eta,where='mid',c=col,ls=ls,lw=2,label=f'h={h:g} m')
                ax.axhline(center,c=PURPLE,ls=':',lw=1)
                ax.set(ylim=(center+1,center-1),xlabel='材料节点 Vs / (m/s)',ylabel='计算深度 η / m',title=f'输入界面 η={center} m附近')
                ax.legend();ax.grid(alpha=.2)
        self.save(fig,'07_interface_node_assignment','层界面离散检查','材料按η<界面逐节点赋值；跳变中点随h变化，是待验证的误差来源之一，不单凭此图证明唯一原因。')
        self.flux_figures()
    def flux_figures(self):
        fig,axs=plt.subplots(2,2,figsize=(13.5,9),layout='constrained');metrics=[]
        for ax,case in zip(axs.flat,CASES):
            folder=self.root/'qualification'/f'REF_{case}_h025';f=np.genfromtxt(folder/'probes/flux.csv',delimiter=',',names=True)
            times=f['actual_time_s']
            row={'case_id':case}
            for name,label,col in [('left','左控制线',RED),('right','右控制线',ORANGE),('bottom','底控制线',PURPLE)]:
                a=f[name+'_outward_W_per_m'];ax.plot(times,a,lw=1.5,c=col,label=label)
                row[name+'_signed_integral_J_per_m']=float(np.trapezoid(a,times))
            metrics.append(row);ax.axhline(0,c='#555555',lw=.6)
            ax.set(xlabel='实际时间 t / s',ylabel='带符号向外功率 / (W/m)',title=LABELS[case],xlim=(0,2.4));ax.ticklabel_format(axis='y',style='sci',scilimits=(0,0),useMathText=True);ax.grid(alpha=.15);ax.legend()
        fig.suptitle('控制线机械功率通量：正值向外，负值向内；不是耗散热',fontsize=16)
        self.save(fig,'08_control_line_flux','控制线功率通量','保留实际时刻和符号；不同控制线的净积分不能直接解释为黏弹性吸收能量。')
        csvout(self.data/'signed_flux_integrals.csv',metrics)
    def wavefields(self):
        schedules={c:table(self.root/'qualification'/f'REF_{c}_h025/snapshots/index.csv') for c in CASES}
        globalamp=0.
        for case in CASES:
            for row in schedules[case]:
                data=read_vts(self.root/'qualification'/f'REF_{case}_h025/snapshots'/row['file'],'Vz',cache_dir=self.cache)
                globalamp=max(globalamp,abs(float(data['full_min'])),abs(float(data['full_max'])))
        dump(self.data/'wavefield_global_scale.json',dict(scope='all four models and all exported reference times',vmax_m_s=globalamp,linthresh_fraction=.001,cmap='coolwarm',display='detail_symlog'))
        for index in range(min(map(len,schedules.values()))):
            values=[];timing=[];amp=0.
            for case in CASES:
                row=schedules[case][index];folder=self.root/'qualification'/f'REF_{case}_h025'
                data=read_vts(folder/'snapshots'/row['file'],'Vz',cache_dir=self.cache)
                a=data['values'];amp=max(amp,abs(float(data['full_min'])),abs(float(data['full_max'])))
                values.append(a);timing.append(dict(case_id=case,**row))
            norm=SymLogNorm(linthresh=1e-3,linscale=.6,vmin=-1,vmax=1,base=10)
            t=float(timing[0]['target_time_s']);fig,axs=plt.subplots(2,2,figsize=(14,5.8),layout='constrained')
            for ax,case,a,row in zip(axs.flat,CASES,values,timing):
                coords,_=self.model(case);x,z=self.domain(ax,coords)
                mesh=ax.pcolormesh(x,z,a/max(globalamp,1e-100),cmap='coolwarm',norm=norm,shading='auto',rasterized=True)
                ax.set_title(f'{LABELS[case]} | 实际t={float(row["actual_time_s"]):.6f} s',fontsize=12)
            fig.suptitle(f'Vz波场 · 目标t={t:g} s · 全时段共同归一化A={globalamp:.3e} m/s\n0.25 m加密参考；物理深宽1∶1；最终收敛未确认',fontsize=14)
            cb=fig.colorbar(mesh,ax=axs,orientation='horizontal',fraction=.045,pad=.045,aspect=65,ticks=[-1,-.1,-.01,0,.01,.1,1]);cb.set_label('Vz / A：detail_symlog，线性阈值10⁻³；所有模型、所有时刻共享尺度')
            self.save(fig,f'wavefield_t{t:.3f}'.replace('.','p')+'_detail_symlog',f'同时刻Vz波场 t={t:g} s','coolwarm；完整原生节点参与绘图，未做空间平滑；所有模型与时刻共享A和对称色限。')
            csvout(self.data/f'wavefield_t{t:.3f}_timing.csv',timing)
            # A full-resolution shallow crop checks details that disappear in the full-domain figure.
            if abs(t-1)<1e-8:
                fig,axs=plt.subplots(2,2,figsize=(14,7),layout='constrained')
                for ax,case,a,row in zip(axs.flat,CASES,values,timing):
                    coords,_=self.model(case);x,z=self.domain(ax,coords,(280,520,-2,85))
                    mesh=ax.pcolormesh(x,z,a/max(globalamp,1e-100),cmap='coolwarm',norm=norm,shading='auto',rasterized=True);ax.set_title(LABELS[case])
                fig.colorbar(mesh,ax=axs,orientation='horizontal',fraction=.04,pad=.03,aspect=65,label='Vz / A；与同一时刻全域图使用相同归一化和色限')
                fig.suptitle('t≈1 s沟谷浅层局部：x=280–520 m，z=−2–85 m，深宽1∶1',fontsize=15)
                self.save(fig,'wavefield_shallow_t1p000_detail_symlog','1 s浅层波场局部','原生0.25 m节点；局部图和全域图共享A与色限，不通过局部自动增益制造异常。')
    def report(self):
        gate=load(self.root/'analysis/full_domain_gate.json');state=load(self.root/'batch_status.json')
        records=self.audit_result
        lines=['# 先导模拟与清晰图件检查报告','',
               '**结论：已有数据完整，但0.5 m候选网格未通过全尺度波形门槛，队列按预设规则停止。当前并非14组先导全部完成。**','',
               f'本次检查：{records["completed"]}个已完成算例（4个研究左炮+11个数值资格算例），{records["not_run"]}个研究算例尚未运行。SHA-256核对：{"已执行" if self.hashes else "未执行"}；发现输入错误{len(records["errors"])}项。',
               '','## 数据与验收','',
               '- 原始Vx/Vz SEG-Y：检查道数、样本数、有限值和几何；完成标志、模型/快照尺寸、快照目标/实际时刻、探针二进制长度和覆盖范围均已核对。',
               '- 四个0.5 m研究算例均完整3 s、6001点、12时刻波场；四个0.25 m参考为2.4 s、4801点、10时刻波场，图件明确使用共同0–2.4 s。',
               '- 0.25 m只是已有加密参考，尚无更细网格证明最终收敛。没有放宽门槛，也没有启动剩余算例。','',
               '| 模型 | 原始Vz L2差 | 15 Hz幅度差中位数 | 原网格门槛 |','| --- | ---: | ---: | --- |']
        for r in gate['grid_comparisons']:lines.append(f'| {LABELS[r["terrain"]+"_"+r["q"]+"_L"]} | {100*r["raw_record_l2"]:.2f}% | {100*r["median_15hz_amplitude_error"]:.2f}% | 未通过 |')
        lines+=['',f'300 m深/60 m PML扩域对照的原始Vz L2残差为{gate["boundary_record_l2"]:.3e}（{100*gate["boundary_record_l2"]:.6g}%），远低于1%门槛。主要停止原因是网格波形差异，当前证据不支持归因于边界反射。','',
                '## 进一步诊断','',
                '- 分频段波形差异见`data/bandwise_grid_errors.csv`；局部互相关时移见`data/trace_delay_diagnostics.csv`。图中保持原时间轴，诊断性时间对齐不用于替换原验收结果。',
                '- 所查主波窗中，0.5 m波形相对0.25 m提前约0.39–5.87 ms；诊断性对齐后窗内L2仍约5.5%–23.7%，不能只归结为单一时间平移。8–15 Hz带通后的四组L2差约18.5%–31.4%，因此也不能把误差简单归结为高频尾部或仅靠缩掉高频解决。',
                '- 原生模型材料用η<界面逐节点赋值，η=10/30/60 m界面两侧的节点跳变中点，在0.5/0.25 m分别相差0.125 m。该O(h)几何采样差、近自由表面降阶和传播频散都可能贡献累计相位差；目前未单独证明哪项是唯一原因。',
                '- 15 Hz幅度差较小不能代表8–30 Hz全频段、相位、混合模态或最终Qs已经合格。下一步应对代表性层状模型做0.25/0.125 m局部及波窗验证，并检验界面表示和近表面误差，再决定续跑参数。','',
                '## 图件清晰度与对比约定','',
                f'- PNG为{self.dpi} dpi，另提供PDF；中文字体微软雅黑，正文/刻度约11–14 pt；完整道集同时给四组矩阵与单模型大图。',
                '- 主要对比采用0.25 m数据。模型turbo；波场coolwarm+detail_symlog；地震记录gray灰度、仅双线性显示插值；计算与定量分析不使用图像平滑。',
                '- 原始道集使用共同物理色限，裁剪比例另表记录；AGC为300 ms RMS、共同下限，仅供事件识别，不用于振幅或Q分析。',
                '- 波场所有时刻、四模型共享全局A及对称色限，detail_symlog线性阈值为10⁻³；完整原生节点参与绘图，深宽1∶1。',
                '- 同时刻图逐面板标注实际时间；局部放大与全域图共享色限，避免局部自动增益造成误导。',
                '- 通量保留符号和实际时间，单位W/m；积分只是控制线净机械功，不等于黏弹性热耗散。','',
                '## 图件目录','', '| 图件 | 内容 |','| --- | --- |']
        for item in self.catalog:lines.append(f'| [{item["title"]}](figures/{item["file"]}) | [PDF](figures/{item["pdf"]})；{item["note"]} |')
        (self.out/'检查报告.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
        html=['<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>先导数据与图件检查</title><style>body{font:17px "Microsoft YaHei",sans-serif;max-width:1280px;margin:35px auto;padding:0 20px;line-height:1.65;color:#20232a}img{max-width:100%;height:auto;border:1px solid #ddd}article{margin:35px 0}h1{color:#ad263c}.notice{background:#fff1df;padding:16px;border-left:5px solid #d97927}a{color:#ad263c}</style><h1>黄土沟谷先导：数据与清晰图件检查</h1><p class="notice">4个研究左炮与11个资格算例完成；0.5 m网格波形门槛未通过，其余10个先导未运行。下图主要为0.25 m参考，最终收敛尚未确认。</p><p><a href="检查报告.md">完整检查报告</a> · PNG可点击查看原始大图，另附PDF。</p>']
        for item in self.catalog:html+=['<article><h2>'+item['title']+'</h2><p>'+item['note']+'</p><a href="figures/'+item['file']+'"><img loading="lazy" src="figures/'+item['file']+'"></a><p><a href="figures/'+item['pdf']+'">PDF</a></p></article>']
        (self.out/'index.html').write_text('\n'.join(html)+'</html>',encoding='utf-8');dump(self.out/'figure_catalog.json',self.catalog)
        policy=REPO/'tools/seismic_plotting/styles/current_publication.json'
        dump(self.out/'display_policy.json',dict(colormaps={'model':'turbo','record':'gray','wavefield':'coolwarm'},style_source=str(policy.relative_to(REPO)),style_source_sha256=sha(policy),wavefield_scope='global across all models and times',wavefield_linthresh_fraction=.001,physical_aspect='equal for models/wavefields',png_dpi=self.dpi,formats=['png','pdf'],record_interpolation='bilinear display only',record_AGC='601 samples, about 300 ms; constant zero extension at edges; common 1% RMS floor; display only'))
        dump(self.out/'review_summary.json',dict(completed_cases=records['completed'],not_run=records['not_run'],audit_errors=records['errors'],figure_count=len(self.catalog),formats=['png','pdf'],png_dpi=self.dpi,simulation_state=state['state'],grid_gate_passed=gate['passed'],boundary_relative_l2=gate['boundary_record_l2']))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--dpi',type=int,default=600);p.add_argument('--skip-hashes',action='store_true');p.add_argument('--skip-wavefields',action='store_true');p.add_argument('--style-only',action='store_true',help='Redraw models/wavefields after a completed audited review; keep existing record/diagnostic figures.')
    args=p.parse_args();out=args.output.resolve();allowed=REPO.parent/'out'
    if not out.is_relative_to(allowed.resolve()):raise ValueError('All generated figures must remain under outer out')
    review=Review(args.root.resolve(),out,args.dpi,not args.skip_hashes)
    if args.style_only:
        review.audit_result=load(out/'integrity_audit.json');old=load(out/'figure_catalog.json')
        review.model_figures();review.wavefields();changed={r['file'] for r in review.catalog}
        combined={r['file']:r for r in old};combined.update({r['file']:r for r in review.catalog})
        review.catalog=[combined[r['file']] for r in old]+[r for r in review.catalog if r['file'] not in {q['file'] for q in old}]
        review.report();print('STYLE REDRAW COMPLETE',out,flush=True);return
    review.audit();review.model_figures();review.record_figures();review.diagnostics()
    if not args.skip_wavefields:review.wavefields()
    review.report();print('REVIEW COMPLETE',out,flush=True)
if __name__=='__main__':main()
