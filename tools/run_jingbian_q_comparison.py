"""Reproduce the 5 km, 2 m, 15 Hz terrain x low-velocity-layer x Q experiment.

Use the current application's CUDA solver, not an attenuation filter. Products
remain under the workspace out/. Build seis2d_experiment before --run.
"""
from pathlib import Path
from datetime import datetime, timezone, timedelta
import argparse, ctypes, hashlib, json, os, shutil, subprocess, sys, zipfile

REPO=Path(__file__).resolve().parents[1]
WORK=REPO.parent; OUT=WORK/'out'
GROUPS=[('M00',0,'01_M00_flat_no_lvl'),('M10',1,'02_M10_terrain_no_lvl'),
        ('M01',2,'03_M01_flat_lvl'),('M11',3,'04_M11_terrain_lvl')]
def now():return datetime.now(timezone(timedelta(hours=8))).isoformat(timespec='seconds')
def digest(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def write(p,d):
 pending=p.with_suffix(p.suffix+'.pending');pending.write_text(json.dumps(d,ensure_ascii=False,indent=2),encoding='utf-8');os.replace(pending,p)
def run(root):
 root.mkdir(parents=True,exist_ok=True)
 exe=OUT/'bin/Release/seis2d_experiment.exe'
 terrain=OUT/'jingbian_15Hz_2m/surface_profile.csv'
 assert exe.is_file() and terrain.is_file()
 spec={'width_m':5000,'depth_m':1500,'spacing_m':2,'global_origin_x_m':2500,'duration_s':1,
       'frequency_hz':15,'reference_hz':15,'fit_min_hz':3,'fit_max_hz':45,'mechanisms':3,
       'solver':'Float64 CUDA, current application core','factorial':'terrain x low-velocity-layer x Q, 2x2x2',
       'field_calibrated_Q':False,'executable_sha256':digest(exe),'original_terrain_sha256':digest(terrain)}
 lock=root/'RUNNING.lock'
 with lock.open('x',encoding='utf-8') as f:f.write(str(os.getpid()))
 state={'state':'preparing','started':now(),'pid':os.getpid(),'specification':spec,'cases':{}}
 try:
  if (root/'producer.json').exists():
   old=json.loads((root/'producer.json').read_text(encoding='utf-8'))
   assert old['specification']==spec,'Input/executable changed; use a new output folder'
  else:
   shutil.copy2(terrain,root/'surface_profile_original.csv')
   sources=['src/two_d/Experiment2D.cpp','src/two_d/CurvilinearSolver2D.cpp','src/two_d/CudaEngine2D.cu','src/two_d/PointKernels2D.hpp','src/two_d/MemoryBudget2D.hpp','src/common/Gsls.cpp','include/SeisCommon/Gsls.hpp','tools/run_jingbian_q_comparison.py']
   source_dir=root/'source';source_dir.mkdir()
   hashes={}
   for name in sources:
    p=REPO/name;target=source_dir/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target);hashes[name]=digest(p)
   with (source_dir/'working_tree.patch').open('wb') as f:subprocess.run(['git','diff','--no-ext-diff'],cwd=REPO,stdout=f,check=True)
   tracked=subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=REPO).decode('utf-8').split('\0')
   with zipfile.ZipFile(source_dir/'source_worktree.zip','w',zipfile.ZIP_DEFLATED) as z:
    for name in dict.fromkeys(tracked):
     p=REPO/name
     if name and p.is_file():z.write(p,'SeisWaveLab_Curvilinear/'+name)
   write(root/'producer.json',{'started':now(),'specification':spec,'source_sha256':hashes,'source_archive_sha256':digest(source_dir/'source_worktree.zip')})
  if shutil.disk_usage(root).free<16*1024**3:raise RuntimeError('Need 16 GiB free output space')
  if os.name=='nt':ctypes.windll.kernel32.SetThreadExecutionState(0x80000001)
  for key,index,baseline in GROUPS:
   for mode in ('elastic','Q'):
    name=key+'_'+mode;folder=root/name
    state.update(state='running',current_case=name,updated=now());write(root/'batch_status.json',state)
    if (folder/'COMPLETE.txt').exists():
     m=json.loads((folder/'run_metadata.json').read_text(encoding='utf-8'))
     assert m['width_m']==5000 and m['depth_m']==1500 and m['duration_s']==1 and m['attenuation']==(mode=='Q')
     state['cases'][name]={'state':'complete','resumed':True};continue
    if folder.exists() and any(folder.iterdir()):raise RuntimeError('Partial case retained; inspect before retry: '+str(folder))
    command=[str(exe),'--group',str(index),'--terrain',str(root/'surface_profile_original.csv'),'--output',str(folder),
             '--width-m','5000','--depth-m','1500','--duration-s','1','--origin-x-m','2500','--frequency','15','--q','on' if mode=='Q' else 'off']
    state['cases'][name]={'state':'running','started':now(),'command':command,'baseline_folder':baseline};write(root/'batch_status.json',state)
    print(now(),'START',name,flush=True)
    with (root/(name+'.log')).open('w',encoding='utf-8') as log:
     code=subprocess.call(command,cwd=WORK,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    if code or not(folder/'COMPLETE.txt').exists():raise RuntimeError(f'{name} failed, exit={code}; inspect its log')
    status=json.loads((folder/'status.json').read_text(encoding='utf-8'))
    assert status['finite'] and status['step']==status['total_steps'] and status['samples']==2001
    write(folder/'sha256.json',{p.relative_to(folder).as_posix():digest(p) for p in folder.rglob('*') if p.is_file() and p.name!='sha256.json'})
    state['cases'][name].update(state='complete',finished=now(),result=status);write(root/'batch_status.json',state)
    print(now(),'COMPLETE',name,'elapsed_s=',round(status['elapsed_s'],1),flush=True)
  state.update(state='simulations_complete',finished=now());write(root/'batch_status.json',state)
  (root/'SIMULATIONS_COMPLETE.txt').write_text('Eight full-duration controlled cases complete. Figures and analysis are separate.\n',encoding='utf-8')
 except BaseException as e:
  state.update(state='failed',error=str(e),updated=now());write(root/'batch_status.json',state);raise
 finally:
  if os.name=='nt':ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
  lock.unlink(missing_ok=True)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',action='store_true');p.add_argument('--output',type=Path,default=OUT/'research/jingbian-q-5km');p.add_argument('--skip-analysis',action='store_true');args=p.parse_args()
 root=args.output.resolve();assert root.is_relative_to(OUT.resolve()) and root!=OUT.resolve()
 if args.run:run(root)
 if not args.skip_analysis:
  subprocess.run([sys.executable,'-B',str(REPO/'tools/analyze_jingbian_q_comparison.py'),'--input',str(root)],check=True)
if __name__=='__main__':main()
