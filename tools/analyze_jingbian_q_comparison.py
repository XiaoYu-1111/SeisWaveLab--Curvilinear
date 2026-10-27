"""Analyze the eight controlled 5 km runs, retaining raw-amplitude comparisons."""
from pathlib import Path
import argparse,csv,hashlib,html,json,struct,subprocess,sys,zipfile
import xml.etree.ElementTree as ET
from datetime import datetime,timezone,timedelta
sys.path.insert(0,str(Path(__file__).resolve().parent/'seismic_plotting'))
from seisplot.publication import style_catalog
DISPLAY_COLORS=style_catalog()['colors']
import numpy as np
from scipy.ndimage import uniform_filter1d
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import Normalize

REPO=Path(__file__).resolve().parents[1];WORK=REPO.parent;OUT=WORK/'out'
KEYS=['M00','M10','M01','M11']
CN=['平地无低速层','起伏无低速层','平地有低速层','起伏有低速层']
EN=['Flat, no LVL','Terrain, no LVL','Flat, LVL','Terrain, LVL']
OLD=['01_M00_flat_no_lvl','02_M10_terrain_no_lvl','03_M01_flat_lvl','04_M11_terrain_lvl']
COLORS=['#243c6a','#0b8d8b','#d88524','#bd4056']
def digest(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def dump(p,d):p.write_text(json.dumps(d,ensure_ascii=False,indent=2),encoding='utf-8')
def table(p,rows):
 with p.open('w',encoding='utf-8-sig',newline='') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
def segy(path):
 traces=[]
 with path.open('rb') as f:
  assert len(f.read(3200))==3200;b=f.read(400)
  assert struct.unpack_from('>H',b,24)[0]==5 and struct.unpack_from('>H',b,16)[0]==500
  while h:=f.read(240):
   assert len(h)==240;n=struct.unpack_from('>H',h,114)[0];payload=f.read(n*4);assert len(payload)==4*n
   traces.append(np.frombuffer(payload,dtype='>f4').astype(float))
 a=np.stack(traces);assert np.isfinite(a).all();return a
def load(root,key,mode):
 folder=root/(key+'_'+mode);assert(folder/'COMPLETE.txt').exists()
 hashes=json.loads((folder/'sha256.json').read_text(encoding='utf-8'))
 for c in ['Vx','Vz']:
  p=folder/'records'/(c+'.sgy');assert digest(p)==hashes['records/'+c+'.sgy']
 raw=np.stack([segy(folder/'records'/(c+'.sgy')) for c in ['Vx','Vz']]);assert raw.shape==(2,126,2001)
 geometry=np.genfromtxt(folder/'records/receiver_geometry.csv',delimiter=',',names=True)
 meta=json.loads((folder/'run_metadata.json').read_text(encoding='utf-8'));status=json.loads((folder/'status.json').read_text(encoding='utf-8'))
 assert status['samples']==2001 and status['finite'] and status['step']==status['total_steps']
 return raw,geometry,meta,status
def save(fig,folder,name):
 for ext in ['png','svg']:fig.savefig(folder/(name+'.'+ext),dpi=240,bbox_inches='tight')
 plt.close(fig)
def read_vts(path,model=False,stride=4):
 case=path.parents[1]
 checks=json.loads((case/'sha256.json').read_text(encoding='utf-8'))
 assert digest(path)==checks[path.relative_to(case).as_posix()],'Model/snapshot changed after run completion'
 doc=ET.parse(path);extent=doc.find('.//Piece').attrib['Extent'].split();nx=int(extent[1])+1;nz=int(extent[5])+1
 arrays={a.attrib.get('Name',''):a for a in doc.findall('.//PointData/DataArray')}
 name='Vp' if model else 'Vz'
 v=np.fromstring(arrays[name].text,sep=' ').reshape(nz,nx)[::stride,::stride].copy()
 xyz=np.fromstring(doc.find('.//Points/DataArray').text,sep=' ').reshape(nz,nx,3)[::stride,::stride].copy()
 return v,xyz
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--input',type=Path,default=OUT/'research/jingbian-q-5km');args=parser.parse_args();root=args.input.resolve()
 assert root.is_relative_to(OUT.resolve()) and(root/'SIMULATIONS_COMPLETE.txt').exists()
 figdir=root/'figures';data=root/'analysis';figdir.mkdir(exist_ok=True);data.mkdir(exist_ok=True)
 plt.rcParams.update({'font.family':'serif','font.serif':['Times New Roman','DejaVu Serif'],'font.size':10,
  'axes.linewidth':.65,'lines.linewidth':.8,'xtick.direction':'in','ytick.direction':'in','svg.fonttype':'none','savefig.facecolor':'white'})
 values=[];geometries=[];metas=[];statuses=[]
 for k in KEYS:
  pairs=[load(root,k,mode) for mode in ['elastic','Q']]
  for member in [1,2]:
   if member==1:assert np.array_equal(pairs[0][member],pairs[1][member])
   else:
    a,b=pairs[0][member].copy(),pairs[1][member].copy()
    for d in [a,b]:d.pop('attenuation');d.pop('budget_host_bytes');d.pop('budget_device_bytes')
    assert a==b,'Pair differs in more than Q and its memory budget'
  values.append([p[0] for p in pairs]);geometries.append([p[1] for p in pairs]);metas.append([p[2] for p in pairs]);statuses.append([p[3] for p in pairs])
 raw=np.asarray(values);t=np.arange(2001)*.0005;offset=geometries[0][0]['offset_m'];xglobal=geometries[0][0]['x_m']+2500
 assert all(np.array_equal(g[0]['x_m'],geometries[0][0]['x_m']) for g in geometries)
 # Keep receivers outside the sponge, focus the comparisons on the central 2 km.
 mask=(np.abs(offset)<=1000)&(geometries[0][0]['main_analysis']==1);assert mask.sum()==50
 np.savez_compressed(data/'原始记录分析数组.npz',raw=raw,time_s=t,offset_m=offset,global_x_m=xglobal,analysis_mask=mask)
 # Same inclusive [0,1] time integration. Spectra use [0,1) with a common Hann window.
 E=np.mean(np.trapezoid(np.sum(raw[:,:,:,mask]**2,axis=2),t,axis=-1),axis=-1)
 spectra=np.mean(np.sum(np.abs(np.fft.rfft(raw[:,:,:,mask,:-1]*np.hanning(2000),axis=-1)*.0005)**2,axis=2),axis=2)
 freq=np.fft.rfftfreq(2000,.0005);band=(freq>=3)&(freq<=45);high=(freq>=15)&(freq<=37.5)
 centroid=np.sum(spectra[:,:,band]*freq[band],axis=-1)/np.sum(spectra[:,:,band],axis=-1)
 rows=[];performance=[]
 for k,key in enumerate(KEYS):
  for m,mode in enumerate(['elastic','Q']):
   a=raw[k,m];late=t>=.5
   ecomp=np.mean(np.trapezoid(a[:,mask]**2,t,axis=-1),axis=-1)
   elate=np.mean(np.trapezoid(np.sum(a[:,mask][:,:,late]**2,axis=0),t[late],axis=-1))
   st=statuses[k][m];mt=metas[k][m]
   rows.append({'case':key+'_'+mode,'label':CN[k],'Q_enabled':bool(m),'velocity_squared_integral_m2_s':float(E[k,m]),
    'RMS_velocity_um_s':float(np.sqrt(E[k,m]/(t[-1]-t[0]))*1e6),'Vz_integral_m2_s':float(ecomp[1]),'Vx_integral_m2_s':float(ecomp[0]),
    'horizontal_fraction':float(ecomp[0]/E[k,m]),'late_window_fraction':float(elate/E[k,m]),'spectral_centroid_3_45Hz':float(centroid[k,m]),
    'record_peak_m_s':float(np.max(np.abs(a[:,mask]))),'Q_to_elastic_integral_ratio':float(E[k,m]/E[k,0]),
    'high_band_15_37p5Hz_Q_to_elastic_ratio':float(np.sum(spectra[k,m,high])/np.sum(spectra[k,0,high]))})
   performance.append({'case':key+'_'+mode,'steps':st['total_steps'],'dt_s':st['dt_s'],'core_compute_s':st['mean_step_ms']*st['total_steps']/1000,
    'elapsed_s_including_exports':st['elapsed_s'],'mean_step_ms':st['mean_step_ms'],'VRAM_budget_GiB':mt['budget_device_bytes']/1024**3,
    'RAM_budget_GiB':mt['budget_host_bytes']/1024**3,'PPW_at_2p5_f0':st['ppw'],'max_Q_fit_relative_error':st['max_q_fit_error'],'traction_relative':st['traction_relative']})
 table(data/'八组原始记录指标.csv',rows);table(data/'计算耗时与资源预算.csv',performance)
 factors=[]
 for m,mode in enumerate(['elastic','Q']):
  factors.append({'Q_state':mode,'LVL_effect_flat_M01_M00':float(E[2,m]/E[0,m]),
   'LVL_effect_terrain_M11_M10':float(E[3,m]/E[1,m]),'terrain_effect_no_LVL_M10_M00':float(E[1,m]/E[0,m]),
   'terrain_effect_LVL_M11_M01':float(E[3,m]/E[2,m]),'quantity':'central 50 receiver mean integral of Vx^2+Vz^2, not total energy'})
 table(data/'地形与低速层的四组因素对比.csv',factors)
 # Check original full-domain records at precisely the same global receivers/time.
 audits=[];legacy=[];baseline_hashes={}
 for k,oldname in enumerate(OLD):
  oldroot=OUT/'jingbian_15Hz_2m'/oldname
  oldchecks=json.loads((oldroot/'sha256.json').read_text(encoding='utf-8'))
  for relative in ['records/Vx.sgy','records/Vz.sgy','records/receiver_geometry.csv','run_metadata.json']:
   p=oldroot/relative;expected=oldchecks.get(relative,oldchecks.get(relative.replace('/','\\')))
   actual=digest(p);assert expected==actual,'Original baseline data changed: '+relative
   baseline_hashes[str(p.relative_to(OUT))]=actual
  g=np.genfromtxt(oldroot/'records/receiver_geometry.csv',delimiter=',',names=True)
  ids=np.asarray([int(np.flatnonzero(np.isclose(g['x_m'],x,rtol=0,atol=1e-9))[0]) for x in xglobal])
  oldraw=np.stack([segy(oldroot/'records'/(c+'.sgy'))[:, :2001] for c in ['Vx','Vz']])[:,ids]
  assert np.allclose(g['z_m'][ids],geometries[k][0]['z_m'],rtol=0,atol=1e-9)
  assert abs(metas[k][0]['source_z_m']-json.loads((oldroot/'run_metadata.json').read_text())['source_z_m'])<1e-10
  legacy.append(oldraw)
  for end in [.5,.75,1.]:
   sel=t<=end;a=oldraw[:,mask][:,:,sel];b=raw[k,0][:,mask][:,:,sel]
   err=float(np.linalg.norm(b-a)/np.linalg.norm(a));corr=float(np.sum(a*b)/(np.linalg.norm(a)*np.linalg.norm(b)))
   audits.append({'case':KEYS[k],'time_end_s':end,'central_receivers':int(mask.sum()),'relative_L2_difference':err,'cosine_similarity':corr,
    'interpretation':'Different outer extent and stable solver dt; measures agreement, not grid convergence.'})
 table(data/'与原10公里无Q实验重叠区核对.csv',audits)
 np.savez_compressed(data/'原10公里重叠记录.npz',raw=np.asarray(legacy),time_s=t,offset_m=offset)
 print('Record metrics and original-domain comparison complete',flush=True)
 # 1. Physical model and trial Q maps, reference velocities retained at 15 Hz.
 fig,axes=plt.subplots(4,2,figsize=(11,12),sharex=True,sharey=True,constrained_layout=True)
 for k in range(4):
  vp,xyz=read_vts(root/(KEYS[k]+'_Q')/'model/model.vts',True)
  eta=np.arange(0,751,4)*2;layers=metas[k][1]['layers'];bounds=np.cumsum([l['thickness_m'] for l in layers[:-1]])
  idx=np.searchsorted(bounds,eta,side='right');qp=np.broadcast_to(np.array([l['qp'] for l in layers])[idx,None],vp.shape)
  for j,(v,cmap,norm) in enumerate([(vp,DISPLAY_COLORS['model'],Normalize(1000,3500)),(qp,DISPLAY_COLORS['trial_q'],Normalize(20,200))]):
   im=axes[k,j].pcolormesh((xyz[:,:,0]+2500)/1000,xyz[:,:,2],v,cmap=cmap,norm=norm,shading='auto',rasterized=True)
   axes[k,j].plot((xyz[0,:,0]+2500)/1000,xyz[0,:,2],color='#1f2e37',lw=.55)
   axes[k,j].scatter([5],[metas[k][1]['source_z_m']],marker='*',s=45,c='red',edgecolors='white',linewidths=.3,zorder=4)
   axes[k,j].set(title=f'{KEYS[k]} | {EN[k]} | '+('Vp at 15 Hz' if j==0 else 'Trial Qp'),ylim=(1500,-250),xlabel='Original profile X (km)',ylabel='Depth Z (m)')
   fig.colorbar(im,ax=axes[k,j],location='bottom',shrink=.85,aspect=35,label='Vp (m/s)' if j==0 else 'Qp')
 save(fig,figdir,'01_四组模型与试验Q值')
 # 2-3. Raw shared amplitude scale and separately marked AGC display.
 vis=np.abs(offset)<=1500;clip=float(np.percentile(np.abs(raw[:,:,1,vis]),99.5));assert clip>0
 for agc in [False,True]:
  fig,axes=plt.subplots(4,2,figsize=(11,12),sharex=True,sharey=True,constrained_layout=True)
  for k in range(4):
   for m in range(2):
    z=raw[k,m,1,vis]
    if agc:
     rms=np.sqrt(uniform_filter1d(z*z,size=601,axis=-1,mode='nearest'));floor=np.max(rms)*1e-4;z=z/np.maximum(rms,floor);lim=3
    else:lim=clip
    im=axes[k,m].imshow(z.T,extent=[offset[vis][0],offset[vis][-1],1,0],aspect='auto',cmap=DISPLAY_COLORS['record'],vmin=-lim,vmax=lim,interpolation='bilinear',interpolation_stage='data')
    axes[k,m].set(title=f'{KEYS[k]} | {EN[k]} | '+('Elastic' if m==0 else 'Qp/Qs enabled'),xlabel='Offset (m)',ylabel='Time (s)')
  fig.colorbar(im,ax=axes,location='bottom',shrink=.65,aspect=55,label='300 ms RMS-AGC, display only' if agc else f'Raw Vz (m/s), common 99.5th-percentile clip: {clip:.3g}')
  fig.suptitle('Vz gathers | '+('AGC for phase visibility; excluded from all metrics' if agc else 'common physical amplitude scale, no AGC'),fontsize=12)
  save(fig,figdir,'03_八组道集_仅显示用AGC' if agc else '02_八组道集_统一原始振幅')
 # 4. Same trace and same spectral window, no per-case normalization.
 fig,axes=plt.subplots(4,2,figsize=(11,10),constrained_layout=True);ri=int(np.argmin(abs(offset-500)))
 for k in range(4):
  for m,color,label in [(0,'#263a48','Elastic'),(1,'#c44c39','Qp/Qs')]:
   axes[k,0].plot(t,raw[k,m,1,ri]*1e6,color=color,label=label)
   axes[k,1].plot(freq,np.sqrt(spectra[k,m]),color=color,label=label)
  axes[k,0].set(title=f'{KEYS[k]} | Vz, offset {offset[ri]:+.0f} m',xlabel='Time (s)',ylabel='Vz (um/s)');axes[k,0].legend(fontsize=8)
  axes[k,1].set(title=f'{KEYS[k]} | central 50 receivers, Vx+Vz',xlabel='Frequency (Hz)',ylabel='RMS Fourier amplitude (m)',xlim=(0,60))
 save(fig,figdir,'04_同道波形与共同窗频谱')
 # 5. Integral ratios versus offset, common central ROI and descriptive windows.
 fig,axes=plt.subplots(2,2,figsize=(11,7.5),constrained_layout=True)
 for k,color in enumerate(COLORS):
  ei=np.trapezoid(np.sum(raw[k]**2,axis=1),t,axis=-1)
  valid=mask&(ei[0]>ei[0].max()*1e-8)
  axes[0,0].plot(offset[valid],ei[1,valid]/ei[0,valid],color=color,label=KEYS[k])
 axes[0,0].axhline(1,color='grey',lw=.6,ls='--');axes[0,0].set(xlabel='Offset (m)',ylabel='Q / elastic integral',title='(a) Receiver velocity-squared integral');axes[0,0].legend()
 axes[0,1].bar(np.arange(4),E[:,1]/E[:,0],color=COLORS);axes[0,1].set(xticks=np.arange(4),xticklabels=KEYS,ylabel='Q / elastic integral',title='(b) Central 50 receivers, 0-1 s')
 width=.33
 for m,color,label in [(0,'#627f91','Elastic'),(1,'#c56b50','Qp/Qs')]:
  axes[1,0].bar(np.arange(4)+(m-.5)*width,centroid[:,m],width,color=color,label=label)
 axes[1,0].set(xticks=np.arange(4),xticklabels=KEYS,ylabel='Spectral centroid (Hz)',title='(c) Common Hann window, 3-45 Hz');axes[1,0].legend()
 core=np.array([p['core_compute_s'] for p in performance]).reshape(4,2)
 axes[1,1].bar(np.arange(4),core[:,1]/core[:,0],color=COLORS);axes[1,1].set(xticks=np.arange(4),xticklabels=KEYS,ylabel='Q / elastic compute time',title='(d) Measured core runtime ratio')
 save(fig,figdir,'05_Q值记录影响与计算代价')
 # 6. Full-domain compatibility check, not a claim of spatial convergence.
 fig,axes=plt.subplots(1,2,figsize=(11,3.8),constrained_layout=True)
 for k,color in enumerate(COLORS):
  a=[r for r in audits if r['case']==KEYS[k]]
  axes[0].semilogy([r['time_end_s'] for r in a],[max(r['relative_L2_difference'],1e-12) for r in a],'-o',ms=3,color=color,label=KEYS[k])
  old=np.asarray(legacy)[k,1,ri]*1e6;axes[1].plot(t,old,color=color,lw=.8,label=KEYS[k]+' original')
  axes[1].plot(t,raw[k,0,1,ri]*1e6,color=color,ls='--',lw=.65)
 axes[0].set(xlabel='Window end (s)',ylabel='Relative L2 difference',title='(a) Local elastic vs original 10 km, central 2 km');axes[0].legend()
 axes[1].set(xlabel='Time (s)',ylabel='Vz (um/s)',title='(b) Offset +500 m: solid original, dashed local');axes[1].legend(fontsize=7)
 save(fig,figdir,'06_与原四组实验的重叠区核对')
 # 7. Snapshot display only: annotate each actual accepted-step time.
 fields=[];snap_times=[]
 for k,key in enumerate(KEYS):
  pair=[];times=[]
  for mode in ['elastic','Q']:
   folder=root/(key+'_'+mode);schedule=list(csv.DictReader((folder/'snapshots/times.csv').open()))
   row=min(schedule,key=lambda d:abs(float(d['target_time_s'])-.5));v,xyz=read_vts(folder/'snapshots'/row['file'])
   pair.append((v,xyz));times.append(float(row['actual_time_s']))
  fields.append(pair);snap_times.append(times)
 spatial=[]
 for pair in fields:
  for v,xyz in pair:
   roi=(xyz[:,:,0]>=1500)&(xyz[:,:,0]<=3500)&(xyz[:,:,2]<=1100);spatial.append(v[roi])
 limit=float(np.percentile(np.abs(np.concatenate(spatial)),99.5))
 fig,axes=plt.subplots(4,2,figsize=(11,11),sharex=True,sharey=True,constrained_layout=True)
 for k in range(4):
  for m in range(2):
   v,xyz=fields[k][m];im=axes[k,m].pcolormesh((xyz[:,:,0]+2500)/1000,xyz[:,:,2],v,cmap=DISPLAY_COLORS['wavefield'],vmin=-limit,vmax=limit,shading='auto',rasterized=True)
   axes[k,m].plot((xyz[0,:,0]+2500)/1000,xyz[0,:,2],color='black',lw=.55)
   axes[k,m].set(title=f'{KEYS[k]} | '+('Elastic' if m==0 else 'Qp/Qs')+f' | t={snap_times[k][m]:.6f} s',xlim=(4,6),ylim=(1100,-250),xlabel='Original profile X (km)',ylabel='Depth Z (m)')
 fig.colorbar(im,ax=axes,location='bottom',shrink=.7,aspect=55,label='Raw Vz (m/s), shared 99.5th-percentile clip; display stride 4, solver h=2 m')
 save(fig,figdir,'07_半秒波场_同尺度对比')
 # 8. Preserve the original four-group comparisons within each Q state.
 factor_names=['LVL_effect_flat_M01_M00','LVL_effect_terrain_M11_M10',
               'terrain_effect_no_LVL_M10_M00','terrain_effect_LVL_M11_M01']
 titles=['LVL effect | flat: M01 / M00','LVL effect | terrain: M11 / M10',
         'Terrain effect | no LVL: M10 / M00','Terrain effect | LVL: M11 / M01']
 fig,axes=plt.subplots(2,2,figsize=(10,7),constrained_layout=True)
 for ax,name,title in zip(axes.flat,factor_names,titles):
  heights=[r[name] for r in factors];ax.bar([0,1],heights,color=['#627f91','#c56b50'],width=.6)
  ax.axhline(1,color='grey',lw=.6,ls='--');ax.set(xticks=[0,1],xticklabels=['Elastic','Qp/Qs'],ylabel='Record integral ratio',title=title)
  for i,value in enumerate(heights):ax.text(i,value,f'{value:.3f}',ha='center',va='bottom',fontsize=9)
  ax.set_ylim(0,max(heights+[1])*1.2)
 save(fig,figdir,'08_原四组因素效应_有无Q对比')
 # Show this experiment's actual 3-45 Hz constitutive fit, using the production
 # fitter. This is a model characterization, not a second analytical benchmark.
 materials=data/'gsls_materials.csv'
 with materials.open('w',encoding='utf-8',newline='') as f:
  w=csv.writer(f);w.writerow(['layer','thickness','vp','vs','rho','qp','qs'])
  for i,l in enumerate(metas[2][1]['layers']):w.writerow([i,l['thickness_m'],l['vp_m_s'],l['vs_m_s'],l['density_kg_m3'],l['qp'],l['qs']])
 fit_exe=OUT/'bin/Release/seis_gsls_fit_curves.exe'
 with (data/'Q拟合导出日志.txt').open('w',encoding='utf-8') as f:
  subprocess.run([str(fit_exe),str(materials),str(data)],check=True,stdout=f,stderr=subprocess.STDOUT)
 fits=np.genfromtxt(data/'gsls_fit_curves.csv',delimiter=',',names=True)
 ref=fits[fits['frequency_hz']==15]
 assert np.max(abs(ref['vp_phase_m_s']/ref['vp_ref_m_s']-1))<1e-12
 assert np.max(abs(ref['vs_phase_m_s']/ref['vs_ref_m_s']-1))<1e-12
 fig,axes=plt.subplots(2,2,figsize=(11,7.5),constrained_layout=True)
 for i,color in enumerate(['#c24b3c','#d48c2e','#71904b','#28818c','#4f5a9d']):
  f=fits[fits['layer']==i];label=f"L{i+1}: {f['target_qp'][0]:g}/{f['target_qs'][0]:g}"
  for ax,name,target in [(axes[0,0],'fitted_qp','target_qp'),(axes[0,1],'fitted_qs','target_qs')]:
   ax.semilogx(f['frequency_hz'],f[name],color=color,label=label);ax.axhline(f[target][0],color=color,ls='--',lw=.5,alpha=.5)
  axes[1,0].semilogx(f['frequency_hz'],f['vp_phase_m_s']/f['vp_ref_m_s'],color=color,label=label)
  axes[1,1].semilogx(f['frequency_hz'],f['vs_phase_m_s']/f['vs_ref_m_s'],color=color,label=label)
 for ax,title,ylabel in zip(axes.flat,['(a) Fitted Qp, dashed targets','(b) Fitted Qs, dashed targets','(c) P phase velocity dispersion','(d) S phase velocity dispersion'],['Qp','Qs','Vp(f) / Vp(15 Hz)','Vs(f) / Vs(15 Hz)']):
  ax.axvline(15,color='grey',lw=.6,ls=':');ax.set(xlabel='Frequency (Hz)',ylabel=ylabel,title=title,xlim=(3,45));ax.legend(fontsize=7)
 save(fig,figdir,'09_试验Q值实际拟合与相速度频散')
 # Explicitly label method and comparison limits in the report.
 final_audit=[r for r in audits if r['time_end_s']==1]
 qrows=[r for r in rows if r['Q_enabled']]
 lines=['# 靖边5 km地形—低速层—Q值波场对照实验','','本轮使用当前程序同一二维曲网格弹性/GSLS核心完成8组模拟。Q值是试验假设，未经工区实测标定。','','## 实验设计','','原四组M00/M10/M01/M11分别为平地无低速层、起伏无低速层、平地有低速层、起伏有低速层。每组增加关闭Q与开启Q两种状态，形成2×2×2因素对照。','','- 从原10 km剖面保留中心X=2.5–7.5 km，未拉伸地形；源仍对应原X=5 km。保留整条原高程数据并平移横坐标，避免改变截取端点的插值斜率。','- 5 km×1.5 km，2501×751=1,878,251个物理节点，h=2 m；15 Hz Ricker爆炸矩源、延迟0.1 s、单位走向长度源矩2×10⁶ N；源计算深度30 m。','- 126道地表接收、40 m道距、0.5 ms记录、2001点、1 s完整时长；分析聚焦中央50道（|偏移距|≤1 km）。','- 地形展平深度1000 m，侧/底吸收64格、强度3，CFL=0.6，Float64 CUDA。开启Q后自动采用其稳定步长；两者记录插值到相同采样时间轴。','- 三支路GSLS；参考频率15 Hz，拟合频带3–45 Hz。速度值解释为该参考频率的相速度。','','## 假设的Qp/Qs分层','','| 介质 | 厚度/m | Vp/Vs (m/s) | 密度(kg/m³) | Qp/Qs |','|---|---:|---|---:|---|','| 表层低速层 | 20 | 1000/500 | 1800 | 20/12.5 |','| 第二低速层 | 40 | 1600/800 | 2000 | 40/25 |','| 第三降速层 | 40 | 2300/1250 | 2200 | 80/50 |','| 上部背景层 | 至计算深度1000 m | 3000/1700 | 2400 | 150/90 |','| 下部背景层 | 1000–1500 m | 3500/2000 | 2550 | 200/120 |','','无低速层模型以上部背景层替代0–100 m低降速带。原配置中2000 m以下的4200/2400 m/s层仍在模型参数中保存，但不进入本轮1.5 km计算范围。','','## 与原四组实验的可比性','','网格间距、原剖面坐标、源、地层参数与接收位置保持一致；模型外边界与稳定dt不同，先核对关闭Q的局部结果与原10 km结果。','','| 组别 | 0–1 s中央50道相对L2差 | 余弦相似度 |','|---|---:|---:|']
 for r in final_audit:lines.append(f"| {r['case']} | {r['relative_L2_difference']:.4%} | {r['cosine_similarity']:.8f} |")
 lines+=['','该核对量化局部模型与原模型在重叠接收/时间范围内的一致性，不是网格收敛试验。细节见analysis/与原10公里无Q实验重叠区核对.csv。','','## Q开启后的记录变化','','指标统一由未经AGC、未经逐道归一化的IEEE32 SEG-Y读回值计算（原始Float64 CSV同时保留）。接收速度平方积分是描述性代理量，不是总机械能或真正信噪比。','', '| 模型 | Q/无Q速度平方积分 | 15–37.5 Hz频带量之比 | 3–45 Hz谱重心/Hz |','|---|---:|---:|---:|']
 for r in qrows:lines.append(f"| {r['case']} | {r['Q_to_elastic_integral_ratio']:.4f} | {r['high_band_15_37p5Hz_Q_to_elastic_ratio']:.4f} | {r['spectral_centroid_3_45Hz']:.3f} |")
 retention=E[:,1]/E[:,0]
 lines+=['',f"本轮假设下，无低速层两组记录积分保留率为{retention[0]:.1%}、{retention[1]:.1%}；含低速层两组为{retention[2]:.1%}、{retention[3]:.1%}。"+
  ('含低速层模型对所设Q值更敏感。' if np.mean(retention[2:])<np.mean(retention[:2]) else '两类模型的Q敏感性需结合逐组结果判断。')+
  '这支持在低速近地表干扰波模拟中检查材料耗散，不能把无Q模型的记录振幅直接当成现场振幅预测。',
  '高频频带指标与谱重心用于检查频谱改变；在完成波型分离前，不将整体记录变弱等同于面波被单独压制或反射信噪比改善。']
 lines+=['','频带量采用共同Hann窗下的Vx/Vz平方傅里叶振幅之和。它与时域积分采用不同的加窗权重，二者不能互作能量守恒检验。地形与薄层的多波干涉会使局部振幅比不单调；不能将某道振幅变大解释为负衰减。','', '所有工况使用相同源矩与子波，并不保证震源向不同介质输入完全相同的机械功。上述比值综合反映该固定震源下的材料耗散、频散、干涉及源介质耦合，不能直接反演为传播路径Q值。','','## 与之前四组因素对照的衔接','','以下沿用原M00/M10/M01/M11定义，分别在无Q与有Q内部计算，衡量地形和低速层效应是否随Q发生变化。比值为同一时窗、同一中央接收区的速度平方积分之比。','','| Q状态 | 平地低速层效应M01/M00 | 起伏低速层效应M11/M10 | 无低速层地形效应M10/M00 | 有低速层地形效应M11/M01 |','|---|---:|---:|---:|---:|']
 for f in factors:lines.append('| '+f['Q_state']+' | '+' | '.join(f"{f[n]:.4f}" for n in factor_names)+' |')
 lines+=['','这些倍数描述的是固定源与接收系统下的记录强弱变化。它们不是介质总能量放大系数，也不能直接推出信噪比提高。','','## 性能与数值检查','','| 工况 | 步数 | 核心耗时/s | 含导出耗时/s | 显存预算/GiB | PPW | 最大Q拟合误差 |','|---|---:|---:|---:|---:|---:|---:|']
 for p in performance:lines.append(f"| {p['case']} | {p['steps']} | {p['core_compute_s']:.2f} | {p['elapsed_s_including_exports']:.2f} | {p['VRAM_budget_GiB']:.2f} | {p['PPW_at_2p5_f0']:.2f} | {p['max_Q_fit_relative_error']:.2%} |")
 lines+=['','显存为程序预分配预算，未作为实测峰值。所有工况完整达到1 s、场值有限、SEG-Y逐样点读回核验通过；已有GSLS解析P/S模态与CPU/CUDA验证在本轮执行前通过。', '', '第09图展示本轮3–45 Hz、参考15 Hz的实际Q拟合与相速度频散，采用与求解器相同的拟合函数。它是本构特征展示，不是独立传播验证。analysis/gsls_branch_coefficients.csv同时保存各支路系数。','','## 图件','','模型与Q采用物理坐标；地表为基准面高程转深度（向下为正）。道集主图统一物理振幅色标，另附300 ms RMS-AGC图仅用于波形识别；所有统计均排除AGC。波场快照使用各自最近达到目标的实际求解时刻，图题注明时间，不据此量化逐像素残差。','']
 for p in sorted(figdir.glob('*.png')):lines+=['![图件](figures/'+p.name+')','']
 lines+=['## 解释边界与后续实验','','- 本轮是局部机制对照，保留1000 m第一目的界面；2000 m界面不在计算域内，1 s后/第二目的层结论需要更深、更长模型。','- 低速层S波/面波的最小PPW偏低，尚未完成细网格收敛；结果适合筛选变化与提出机制假说，不能据此发布精确面波衰减率。','- 保留原40 m道距保证本轮接收位置与旧实验可对应，但不保证低视速度波的空间采样充分。例如500 m/s的波在15 Hz时波长约33 m，40 m道距不能满足其空间无混叠采样。后续定量分析面波频散/衰减应另用5–10 m虚拟道距，并保留原40 m道集作抽取对照；不能靠图像插值补回采样信息。','- 0.5–1 s窗口仅称晚窗，未自动分离反射波、面波、散射波。要评价有效波与干扰波，应增加分量/视速度/相位标记与独立标定。','- 未加入实测炮集，Q值、源子波、耦合和密度仍需标定。加入Q描述了材料耗散，不保证单凭这一因素改善实测拟合。','- 后续可固定M11进行Qp/Qs分别变化、井深变化、0.5/1/2 m局部收敛及边界距离敏感性；拓展到原10 km域需要更大显存或优化存储。','','## 重现','','从SeisWaveLab_Curvilinear运行：`python -B tools/run_jingbian_q_comparison.py --run`；只重绘与分析时去掉`--run`。需先构建seis2d_experiment。','', '原始model/model.vts可在当前程序二维界面导入；其中包含地形、地层与Q开关。各工况包含Vx/Vz SEG-Y、Float64 CSV、四个快照和SHA256。source/source_worktree.zip保存本轮求解开始时源代码，producer.json保存二进制与输入指纹。分析脚本另存source/analysis，避免声称运行前代码归档已包含之后撰写的分析代码。','']
 (root/'实验报告.md').write_text('\n'.join(lines),encoding='utf-8')
 manifest={'scope':'5 km local terrain x low-velocity-layer x Q factorial synthetic experiment','field_Q_calibrated':False,
  'analysis_script_sha256':digest(Path(__file__)),'Q_fit_export_executable_sha256':digest(fit_exe),'original_baseline_sha256':baseline_hashes,
  'records':rows,'factor_effects':factors,'performance':performance,'original_domain_audit':audits,'raw_gather_clip_99p5_m_s':clip,'snapshot_times_s':snap_times,
  'analysis_roi':'50 receivers, |offset|<=1000 m; 0..1 s','analysis_precision':'IEEE32 SEG-Y readback, raw Float64 CSV preserved',
  'hashes':{str(p.relative_to(root)):digest(p) for p in list(data.glob('*')) if p.is_file()}}
 dump(root/'analysis_manifest.json',manifest)
 # An independent self-contained reading page, with links to re-importable model files.
 body=['<h1>靖边5 km Q值波场对照实验</h1><p>地形 × 低速层 × Q，共8组 · 2 m网格 · 15 Hz · 1 s</p>',
  '<p><strong>已完成计算与分析。Q为待标定试验值；本轮不覆盖第二目的层。</strong></p>',
  '<p><a href="实验报告.md">完整中文报告</a> · <a href="analysis/八组原始记录指标.csv">记录指标CSV</a> · <a href="analysis/计算耗时与资源预算.csv">性能CSV</a> · <a href="analysis/与原10公里无Q实验重叠区核对.csv">原实验核对CSV</a></p>',
  '<h2>导入当前二维程序的模型</h2><ul>']
 for k in KEYS:
  for mode in ['elastic','Q']:body.append(f'<li><a href="{k}_{mode}/model/model.vts">{k} · {"关闭Q" if mode=="elastic" else "启用Q"}</a></li>')
 body+=['</ul><h2>对照图件</h2>']
 for p in sorted(figdir.glob('*.png')):body.append(f'<h3>{html.escape(p.stem)}</h3><a href="figures/{p.stem}.svg">SVG矢量版</a><img src="figures/{p.name}" alt="{html.escape(p.stem)}">')
 page='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>靖边5 km Q值实验</title><style>body{font:16px/1.7 "Microsoft YaHei",sans-serif;background:#f3f6f8;color:#213747;margin:0}main{max-width:1200px;margin:auto;padding:32px 24px;background:white}a{color:#006c92}img{display:block;width:100%;height:auto;margin:14px 0 30px;border:1px solid #dfe5e9}h1{font-size:28px}h2{margin-top:30px}</style><main>'+''.join(body)+'</main></html>'
 (root/'index.html').write_text(page,encoding='utf-8')
 analysis_source=root/'source/analysis';analysis_source.mkdir(exist_ok=True)
 import shutil
 shutil.copy2(Path(__file__),analysis_source/Path(__file__).name)
 shutil.copy2(REPO/'docs/QP_QS_VISCOELASTIC_2D.md',analysis_source/'Qp_Qs方法说明.md')
 shutil.copy2(REPO/'tools/export_gsls_fit_curves.cpp',analysis_source/'export_gsls_fit_curves.cpp')
 shutil.copy2(REPO/'cmake/Targets.cmake',analysis_source/'Targets.cmake')
 (analysis_source/'复现补充说明.md').write_text('本目录是求解开始后补充的分析代码与构建目标。source_worktree.zip保留求解开始时的源树。重建全部分析时，将本目录的analyze_jingbian_q_comparison.py及export_gsls_fit_curves.cpp置于解压源码tools目录，将Targets.cmake置于cmake目录，再构建seis_gsls_fit_curves。拟合导出调用与求解器相同的GSLS拟合函数，只用于展示实验频带的本构曲线，不作为独立传播验证。\n',encoding='utf-8')
 summary_zip=root.parent/'靖边5公里_Q值波场对照_图件与分析.zip'
 files=[root/'实验报告.md',root/'index.html',root/'producer.json',root/'analysis_manifest.json']+list(figdir.glob('*'))+list(data.glob('*'))+list(analysis_source.glob('*'))
 with zipfile.ZipFile(summary_zip,'w',zipfile.ZIP_DEFLATED) as z:
  for p in files:
   # Summary bundle omits large model/snapshot files; make that clear in its page.
   if p.name=='index.html':
    text=p.read_text(encoding='utf-8');start=text.index('<h2>导入当前二维程序的模型</h2>');end=text.index('<h2>对照图件</h2>');text=text[:start]+'<p>本包包含图件与分析。完整VTS模型、快照、SEG-Y/Float64 CSV位于工作区原实验目录。</p>'+text[end:];z.writestr('靖边5公里Q值实验/'+p.relative_to(root).as_posix(),text)
   else:z.write(p,'靖边5公里Q值实验/'+p.relative_to(root).as_posix())
 with zipfile.ZipFile(summary_zip) as z:assert z.testzip() is None
 (root/'ANALYSIS_COMPLETE.txt').write_text('Eight cases analyzed; originals audited; figures and summary archive generated.\n',encoding='utf-8')
 print(json.dumps({'figures':len(list(figdir.glob('*.png'))),'case_count':8,'max_original_L2':max(r['relative_L2_difference'] for r in final_audit),
  'Q_integral_ratios':{r['case']:r['Q_to_elastic_integral_ratio'] for r in qrows},'summary_zip':str(summary_zip)},ensure_ascii=False),flush=True)
if __name__=='__main__':main()
