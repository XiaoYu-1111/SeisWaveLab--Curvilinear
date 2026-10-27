"""Redraw exported inversion models and curve fits; never execute an inversion."""
import json,re
import numpy as np
from matplotlib import pyplot as plt
from .core import setup,resolve,save,axes_grid,write_rows,PALETTE
from .io import table,numeric_csv
from .publication import resolve_style

class InversionStudy:
    def __init__(self,cfg):
        cfg=resolve_style(cfg);self.cfg=cfg;self.out=setup(cfg);self.truth=np.asarray(cfg['truth']['vs_m_s'],float)
        self.input_dir=resolve(cfg.get('input_dir','.'))
        self.true_h=np.asarray(cfg['truth']['thickness_m'],float);self.initial=np.asarray(cfg['initial_vs_m_s'],float)
        self.free=np.asarray(cfg['free_vs'],bool);self.n=len(self.truth)
        if len(self.true_h)!=self.n-1 or len(self.initial)!=self.n or len(self.free)!=self.n:raise ValueError('Truth/initial/fixed flag layer counts differ')
        if min(self.truth)<=0 or min(self.initial)<=0 or (len(self.true_h) and min(self.true_h)<=0):raise ValueError('Layer parameters must be positive')
        self.rows={};self.idcol=cfg.get('id_column','id');self.rmsecol=cfg.get('rmse_column','rmse')
        for path in cfg['results_csv']:
            for row in table(resolve(path,self.input_dir)):
                key=row[self.idcol]
                if key in self.rows:raise ValueError(f'Duplicate inversion id: {key}')
                if row.get('success','1') not in ('1','1.0','true','True'):raise ValueError(f'Failed inversion row: {key}')
                self.rows[key]=row
        self.main=self.rows[cfg['main_id']]
        self.vcols=cfg.get('vs_columns',[f'vs{i+1}' for i in range(self.n)])
        self.hcols=cfg.get('thickness_columns',[f'h{i+1}' for i in range(self.n-1)])
        for row in self.rows.values():
            v=self.vs(row)
            if len(v)!=self.n or not np.isfinite(v).all() or min(v)<=0:raise ValueError('Invalid inverted velocity vector')

    def vs(self,row):return np.array([float(row[c]) for c in self.vcols])
    def thickness(self,row):return np.array([float(row[c]) for c in self.hcols]) if all(c in row for c in self.hcols) else self.true_h
    def rmse(self,row):return float(row[self.rmsecol])

    def profile(self,ax,v,h,label,color,lw=1.8,ls='-'):
        depth=max(self.cfg.get('depth_max_m',100),float(sum(h))+1)
        ax.step(np.r_[v,v[-1]],np.r_[0,np.cumsum(h),depth],where='pre',label=label,color=color,lw=lw,ls=ls)
        ax.set(xlabel='Vs / (m/s)',ylabel='深度 / m',ylim=(depth,0));ax.grid(alpha=.15)

    def plot_models(self):
        fig,axs=plt.subplots(1,2,figsize=(13,6),layout='constrained')
        bounds=np.r_[0,np.cumsum(self.true_h),self.cfg.get('depth_max_m',100)]
        for ax in axs:
            for i,free in enumerate(self.free):
                if not free:ax.axhspan(bounds[i],bounds[i+1],color='#dddddd',alpha=.35)
            self.profile(ax,self.truth,self.true_h,'正演真值模型','#222222',2.4)
            self.profile(ax,self.initial,self.true_h,'反演起始模型','#888888',1.6,'--')
            self.profile(ax,self.vs(self.main),self.thickness(self.main),'反演结果','#c00000',2)
        axs[0].set_title('全深度对比，灰底参数固定');axs[1].set_title('浅层放大')
        axs[1].set_ylim(self.cfg.get('shallow_depth_m',35),0)
        if self.cfg.get('shallow_vs_limits_m_s'):axs[1].set_xlim(*self.cfg['shallow_vs_limits_m_s'])
        axs[0].legend(fontsize=10)
        rows=[dict(layer=i+1,true_vs_m_s=float(self.truth[i]),initial_vs_m_s=float(self.initial[i]),inverted_vs_m_s=float(self.vs(self.main)[i]),
             relative_error_pct=float(100*(self.vs(self.main)[i]/self.truth[i]-1)) if self.free[i] else '',status='free' if self.free[i] else 'fixed_prior') for i in range(self.n)]
        write_rows(self.out/'data/recovered_layers.csv',rows)
        return save(fig,self.out,'inversion_model_recovery',self.cfg)

    def plot_curve_fit(self):
        if not self.cfg.get('fit_csv'):return []
        a=np.atleast_1d(numeric_csv(resolve(self.cfg['fit_csv'],self.input_dir)))
        columns=self.cfg.get('fit_columns',{});f=a[columns.get('frequency','frequency_hz')];observed=a[columns.get('observed','observed_m_s')];fitted=a[columns.get('fitted','fitted_m_s')]
        initial_col=columns.get('initial','initial_m_s');rmse=float(np.sqrt(np.mean((observed-fitted)**2)))
        if not np.isclose(rmse,self.rmse(self.main),rtol=1e-5,atol=1e-6):raise ValueError('Fit CSV RMSE does not match selected model row')
        fig,axs=plt.subplots(1,2,figsize=(13,5),layout='constrained')
        axs[0].plot(f,observed,'o',color='#ed7d31',ms=4,label='记录提取曲线')
        if initial_col in a.dtype.names:axs[0].plot(f,a[initial_col],'--',color='#888888',label='反演起始模型正演')
        axs[0].plot(f,fitted,color='#c00000',label='反演结果正演',lw=2)
        if self.cfg.get('theory_curve_csv'):
            theory=np.atleast_1d(numeric_csv(resolve(self.cfg['theory_curve_csv'],self.input_dir)))
            cols=self.cfg.get('theory_columns',['frequency_hz','mode0_c_m_s']);axs[0].plot(theory[cols[0]],theory[cols[1]],':',color='#222222',label='正演真值模型理论')
        axs[0].set(xlabel='频率 / Hz',ylabel='相速度 / (m/s)',xlim=(min(f),max(f)));axs[0].legend(fontsize=10)
        axs[1].plot(f,fitted-observed,'o-',color='#c00000',label=f'RMSE {rmse:.3f}m/s')
        axs[1].axhline(0,color='#888888',lw=.8);axs[1].set(xlabel='频率 / Hz',ylabel='计算 − 提取 / (m/s)');axs[1].legend()
        return save(fig,self.out,'inversion_dispersion_fit',self.cfg)

    def plot_multistart(self):
        groups=self.cfg.get('multistart_groups',[])
        if not groups:return []
        fig,axs=axes_grid(len(groups),2,5.5)
        for ax,group in zip(axs,groups):
            self.profile(ax,self.truth,self.true_h,'正演真值模型','#222222',2.4)
            for j,key in enumerate(group['ids']):
                r=self.rows[key];self.profile(ax,self.vs(r),self.thickness(r),f'{group.get("labels",group["ids"])[j]}，RMSE {self.rmse(r):.3f}',PALETTE[j%len(PALETTE)])
            ax.set_title(group['title']);ax.legend(fontsize=9)
        return save(fig,self.out,'inversion_multistart',self.cfg)

    def plot_thickness_scan(self):
        opt=self.cfg.get('thickness_scan',{});prefixes=opt.get('id_prefixes',[])
        rows=[r for key,r in self.rows.items() if key in opt.get('ids',[]) or any(key.startswith(p) for p in prefixes)]
        if not rows:return []
        layer=int(opt.get('layer_index',0));v_layer=int(opt.get('velocity_layer_index',min(1,self.n-1)))
        rows.sort(key=lambda r:self.thickness(r)[layer]);best=min(rows,key=self.rmse)
        fig,axs=plt.subplots(1,2,figsize=(13,5),layout='constrained')
        h=np.array([self.thickness(r)[layer] for r in rows]);e=np.array([self.rmse(r) for r in rows])
        axs[0].plot(h,e,'o-',color='#c00000',ms=3);axs[0].axvline(self.true_h[layer],color='#222222',ls='--',label='真值厚度')
        axs[0].plot(self.thickness(best)[layer],self.rmse(best),'*',color='#ed7d31',ms=12,label='扫描最小值')
        axs[0].set(xlabel=f'第{layer+1}层厚度 / m',ylabel='拟合RMSE / (m/s)');axs[0].legend()
        axs[1].plot(h,[self.vs(r)[v_layer] for r in rows],'o-',color='#ed7d31',ms=3)
        axs[1].axhline(self.truth[v_layer],color='#222222',ls='--')
        axs[1].set(xlabel=f'第{layer+1}层厚度 / m',ylabel=f'第{v_layer+1}层Vs / (m/s)')
        (self.out/'data/best_thickness_scan.json').write_text(json.dumps(best,indent=2),encoding='utf-8')
        return save(fig,self.out,'inversion_thickness_tradeoff',self.cfg)

    def plot_sensitivity(self):
        cases=self.cfg.get('sensitivity_cases',[])
        if not cases:return []
        layers=self.cfg.get('sensitivity_layers',[i for i,v in enumerate(self.free) if v]);n=len(layers)
        fig,ax=plt.subplots(figsize=(12,5),layout='constrained');x=np.arange(len(cases));width=.8/max(n,1)
        for j,layer in enumerate(layers):
            vals=[100*(self.vs(self.rows[c['id']])[layer]/self.truth[layer]-1) for c in cases]
            ax.bar(x+(j-(n-1)/2)*width,vals,width,color=PALETTE[j%len(PALETTE)],label=f'第{layer+1}层Vs偏差')
        ax.axhline(0,color='#888',lw=.8);ax.set(xticks=x,xticklabels=[c['label'] for c in cases],ylabel='相对正演真值偏差 / %');ax.legend()
        return save(fig,self.out,'inversion_recovery_sensitivity',self.cfg)
