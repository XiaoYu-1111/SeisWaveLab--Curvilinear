"""Prepare/run the finite, gated loess pilot queue; all artifacts stay in outer out."""
from pathlib import Path
import argparse, csv, ctypes, datetime as dt, hashlib, json, os, shutil, subprocess, sys, time
sys.dont_write_bytecode=True
import numpy as np
REPO=Path(__file__).resolve().parents[1]
WORK=REPO.parent
OUT=WORK/'out'
DESIGN=OUT/'research_design/loess_scattering_q_20261029'
EXE=OUT/'bin/Release/seis2d_loess_pilot.exe'
sys.path.insert(0,str(REPO/'tools/seismic_plotting'))
from seisplot.io import read_segy

def now(): return dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(timespec='seconds')
def write(file,obj):
    file=Path(file);pending=file.with_suffix(file.suffix+'.pending')
    pending.write_text(json.dumps(obj,ensure_ascii=False,indent=2),encoding='utf-8')
    for attempt in range(10):
        try:os.replace(pending,file);return
        except PermissionError:
            if attempt==9:raise
            time.sleep(.1)
def digest(file):
    h=hashlib.sha256()
    with Path(file).open('rb') as f:
        for b in iter(lambda:f.read(4*1024*1024),b''):h.update(b)
    return h.hexdigest()
def under_out(root):
    root=root.resolve()
    if not root.is_relative_to(OUT.resolve()) or root==OUT.resolve():raise ValueError('Output must be a subdirectory of outer out')
    return root
def config(case_id,terrain,state,shot,h=.5,width=800,depth=200,duration=3,pml=40,uniform=False,qualification=False,cfl=.6):
    local=qualification
    source=50 if local else 150 if shot=='L' else 650
    receiver=70 if local else 200
    count=121 if local else 401
    layers=[(0,550,250,1550,40,25)] if uniform else [(10,550,250,1550,40,25),(20,800,350,1800,80,50),(30,1500,700,2050,120,75),(0,2500,1300,2250,200,125)]
    if state!='E' and not uniform:
        q=float(state[1:]);layers[0]=(10,550,250,1550,1.6*q,q)
    terrain_file=DESIGN/('terrain_'+terrain+'.csv')
    if local:
        terrain_file=ROOT_TMP/'configs/local_flat.csv'
    snapshots=[.2,.6,duration] if local else [.1,.2,.35,.5,.65,.8,1.,1.25,1.5,2.,2.5,3.]
    snapshots=sorted(set(t for t in snapshots if t<=duration))
    lines=['LOESS_PILOT 1','id '+case_id,f'grid {width} {depth} {h}',f'time 15 {duration} {cfl} 0.5',
           f'source {source} 2 100 0.5 1.5',f'receivers {receiver} {count} 1 0',
           f'terrain {terrain_file.as_posix()} 0 {min(150,depth)}','layers '+str(len(layers))]
    lines+=[' '.join(map(str,l)) for l in layers]
    lines += [f'attenuation {int(state!="E")} 3 3 45 15',f'boundary {pml} 1e-5 3 15',
              'snapshots '+str(len(snapshots))+' '+' '.join(map(str,snapshots)),
              'probes 300 500 80 1 1' if not local else 'probes 80 160 60 1 0']
    return '\n'.join(lines)+'\n'

ROOT_TMP=None
def prepare(root):
    global ROOT_TMP
    ROOT_TMP=root
    if root.exists() and any(root.iterdir()):raise FileExistsError('Refusing to replace an existing session')
    for name in ['configs','logs','preflight','qualification','pilot','source','analysis']: (root/name).mkdir(parents=True,exist_ok=True)
    (root/'configs/local_flat.csv').write_text('x_m,elevation_m\n0,0\n240,0\n',encoding='ascii')
    entries=[]
    def add(case,folder,**kwargs):
        file=root/'configs'/f'{case}.cfg';file.write_text(config(case,**kwargs),encoding='utf-8')
        entry=dict(case_id=case,config=str(file),output=str(root/folder/case),execution_state='queued',**kwargs)
        entries.append(entry);return entry
    for q in ['E','Q25']:
        for h in [1.,.5,.25]:add(f'G_U_{q}_h{int(round(h*100)):03d}','qualification',terrain='F',state=q,shot='L',h=h,width=240,depth=120,duration=1.2,uniform=True,qualification=True)
    pilots={}
    # Four real pilot cases double as full-domain qualification data.
    for terrain in ['F','H20']:
        for q in ['E','Q25']:
            case=f'{terrain}_{q}_L';pilots[case]=add(case,'pilot',terrain=terrain,state=q,shot='L')
    for terrain in ['F','H20']:
        for q in ['E','Q25']:
            add(f'REF_{terrain}_{q}_L_h025','qualification',terrain=terrain,state=q,shot='L',h=.25,duration=2.4)
    add('BOUND_H20_Q25_L','qualification',terrain='H20',state='Q25',shot='L',depth=300,pml=60)
    # Remaining ten scientific pilot cases, including symmetry and uniform controls.
    for terrain in ['F','H20']:
        for q in ['E','Q25']:
            case=f'{terrain}_{q}_R';pilots[case]=add(case,'pilot',terrain=terrain,state=q,shot='R')
    for terrain in ['F','H20']:
        for shot in ['L','R']:
            case=f'U_{terrain}_E_{shot}';pilots[case]=add(case,'pilot',terrain=terrain,state='E',shot=shot,uniform=True)
    for shot in ['L','R']:
        case=f'F_Q50_{shot}';pilots[case]=add(case,'pilot',terrain='F',state='Q50',shot=shot)
    # ASYM gets a no-time-stepping geometry/budget check, not an unrequested scientific run.
    asym=root/'configs/PREFLIGHT_ASYM_Q25.cfg'
    asym.write_text(config('PREFLIGHT_ASYM_Q25','ASYM','Q25','L'),encoding='utf-8')
    source_files=['include/Seis2D/CurvilinearSolver2D.hpp','src/two_d/LoessPilot2D.cpp','src/two_d/LoessPilotValidation2D.cpp',
                  'src/two_d/CurvilinearSolver2D.cpp','src/two_d/CudaEngine2D.cu','src/two_d/CudaEngine2D.hpp','src/two_d/PointKernels2D.hpp',
                  'src/two_d/ModelIo2D.cpp','src/common/Gsls.cpp','tools/run_loess_pilot.py']
    hashes={name:digest(REPO/name) for name in source_files}
    for name in source_files:
        dst=root/'source'/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(REPO/name,dst)
    (root/'source/worktree.patch').write_bytes(subprocess.check_output(['git','diff','--no-ext-diff'],cwd=REPO))
    plan=dict(status='prepared_not_started',created=now(),root=str(root),executable=str(EXE),executable_sha256=digest(EXE),
              git_HEAD=subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),source_sha256=hashes,
              cases=entries,pilot_case_count=len(pilots),qualification_case_count=len(entries)-len(pilots),asym_preflight=str(asym),
              duration_note='Pilot=3 s/6001 samples; fine-grid references=2.4 s/4801 samples; qualification is not a pilot completion certificate.',
              snapshot_note='12 target times per pilot; actual times recorded separately.',
              probe_format='fields_f64.bin: 8-byte SWP5F64 newline magic, uint32 little-endian node count; frames: float64 actual time + [node,5] float64 Vx,Vz,Sxx,Szz,Sxz. geometry.csv and index.csv define nodes/times.')
    write(root/'plan.json',plan);write(root/'batch_status.json',dict(state='prepared_not_started',created=now(),pilot_total=len(pilots),pilot_complete=0))
    write(root.parent/'latest_session.json',dict(root=str(root),state='prepared_not_started'))
    print(str(root),flush=True)

def rayleigh_reference(f,viscous):
    rho=1550.
    if viscous:
        b=np.genfromtxt(DESIGN/'gsls_fit/gsls_branch_coefficients.csv',delimiter=',',names=True);b=b[b['layer']==1]
        s=2j*np.pi*f
        M=b['M_relaxed_Pa'][0]+np.sum(b['delta_M_Pa']*s/(s+b['rate_rad_s']))
        mu=b['mu_relaxed_Pa'][0]+np.sum(b['delta_mu_Pa']*s/(s+b['rate_rad_s']))
    else:M=rho*550**2;mu=rho*250**2
    beta=mu/M
    roots=np.roots([1,-8,24-16*beta,-16*(1-beta)])
    physical=min((r for r in roots if 0<r.real<1),key=lambda r:abs(r-.85))
    velocity=np.sqrt(mu/rho)*np.sqrt(physical)
    k=2*np.pi*f/velocity
    return dict(phase_velocity_m_s=float(2*np.pi*f/k.real),alpha_m_inv=float(-k.imag))

def spectrum(case,halfwindow=.16):
    gather=read_segy(case/'records/Vz.sgy',case/'geometry/receivers.csv',source_x_m=50)
    offsets=gather.x-50;chosen=(offsets>=80)&(offsets<=140)
    x=offsets[chosen];data=gather.values[chosen];t=gather.t
    center=.1+x/231.
    q=(t[None,:]-center[:,None])/halfwindow
    taper=np.where(abs(q)<1,.5*(1+np.cos(np.pi*np.clip(q,-1,1))),0.)
    ft=np.fft.rfft(data*taper,n=16384,axis=1);freq=np.fft.rfftfreq(16384,gather.dt)
    use=(freq>=10)&(freq<=25)
    phase=np.unwrap(np.angle(ft[:,use]),axis=0);amplitude=np.maximum(abs(ft[:,use]),1e-100)
    phase_slope=np.polyfit(x,phase,1)[0];alpha=-np.polyfit(x,np.log(amplitude),1)[0]
    return freq[use],-2*np.pi*freq[use]/phase_slope,alpha

def uniform_gate(root):
    result={'reference':'Independent complex Rayleigh cubic using fitted GSLS moduli; 80-140 m offsets; fixed moving cosine window halfwidth 0.16 s.','results':[]}
    measured={}
    for q in ['E','Q25']:
        for h in [1.,.5,.25]:
            f,c,a=spectrum(root/'qualification'/f'G_U_{q}_h{int(round(h*100)):03d}')
            index=int(np.argmin(abs(f-15)))
            ref=rayleigh_reference(float(f[index]),q!='E')
            row=dict(q=q,h=h,frequency_hz=float(f[index]),phase_velocity_m_s=float(c[index]),alpha_m_inv=float(a[index]),reference=ref)
            result['results'].append(row);measured[q,h]=row
    reference_alpha=measured['Q25',.25]['reference']['alpha_m_inv']
    checks={}
    for q in ['E','Q25']:
        fine,base=measured[q,.25],measured[q,.5]
        checks[q+'_fine_phase_error']=abs(fine['phase_velocity_m_s']/fine['reference']['phase_velocity_m_s']-1)<.01
        checks[q+'_grid_phase_difference']=abs(base['phase_velocity_m_s']/fine['phase_velocity_m_s']-1)<.01
        checks[q+'_grid_alpha_difference']=abs(base['alpha_m_inv']-fine['alpha_m_inv'])<.1*reference_alpha
    checks['Q25_fine_analytic_alpha']=abs(measured['Q25',.25]['alpha_m_inv']/reference_alpha-1)<.1
    checks['elastic_spurious_alpha']=abs(measured['E',.25]['alpha_m_inv'])<.1*reference_alpha
    result.update(checks=checks,passed=all(checks.values()))
    write(root/'analysis/uniform_gate.json',result)
    if not result['passed']:raise RuntimeError('Uniform Rayleigh qualification failed; inspect analysis/uniform_gate.json. Remaining queue stopped.')
    return result

def full_domain_gate(root):
    # Compare a fixed receiver/time region, preserving amplitudes and the actual raw sampling axis.
    rows=[];passed=True
    for terrain in ['F','H20']:
        for q in ['E','Q25']:
            base=read_segy(root/'pilot'/f'{terrain}_{q}_L/records/Vz.sgy',root/'pilot'/f'{terrain}_{q}_L/geometry/receivers.csv')
            fine=read_segy(root/'qualification'/f'REF_{terrain}_{q}_L_h025/records/Vz.sgy',root/'qualification'/f'REF_{terrain}_{q}_L_h025/geometry/receivers.csv')
            selected=(base.x>=230)&(base.x<=580)
            a=base.values[selected,:fine.values.shape[1]];b=fine.values[selected]
            # Full Vz waveform norm is deliberately reported as well as narrowband ratios.
            l2=float(np.linalg.norm(a-b)/max(np.linalg.norm(b),1e-100))
            frequencies=np.fft.rfftfreq(16384,base.dt);at=int(np.argmin(abs(frequencies-15)))
            offsets=base.x[selected]-150;centers=.1+offsets/240
            window=np.abs(base.t[:b.shape[1]][None,:]-centers[:,None])<.22
            aa=abs(np.fft.rfft(a*window,n=16384,axis=1))[:,at];bb=abs(np.fft.rfft(b*window,n=16384,axis=1))[:,at]
            usable=bb>max(bb.max()*1e-3,1e-30)
            ratios=aa[usable]/bb[usable];ratio_error=float(np.median(abs(ratios-1)))
            okay=l2<.05 and ratio_error<.05
            rows.append(dict(terrain=terrain,q=q,raw_record_l2=l2,median_15hz_amplitude_error=ratio_error,usable_traces=int(usable.sum()),passed=okay));passed &= okay
    ref=root/'qualification/BOUND_H20_Q25_L';base_dir=root/'pilot/H20_Q25_L'
    a=read_segy(base_dir/'records/Vz.sgy').values;b=read_segy(ref/'records/Vz.sgy').values
    residual=float(np.linalg.norm(a-b)/max(np.linalg.norm(b),1e-100))
    passed &= residual<.01
    result=dict(passed=bool(passed),grid_comparisons=rows,boundary_record_l2=residual,
                note='Fine grid compares 0-2.4 s; late 2.4-3 s pilot coda remains outside this convergence qualification. This gate is exploratory and does not certify every frequency/window or amplitude-derived Q.')
    write(root/'analysis/full_domain_gate.json',result)
    if not passed:raise RuntimeError('Full-domain grid/boundary qualification failed; do not launch remaining pilot cases.')
    return result

def run(root):
    plan=json.loads((root/'plan.json').read_text(encoding='utf-8'))
    if digest(EXE)!=plan['executable_sha256']:raise RuntimeError('Executable changed since preparation; freeze a new session')
    lock=root/'RUNNING.lock'
    with lock.open('x',encoding='ascii') as f:f.write(str(os.getpid()))
    state=dict(state='preflight',started=now(),controller_pid=os.getpid(),pilot_total=14,pilot_complete=0,qualification_complete=0,current_case=None)
    child=None
    awake=False
    try:
        if os.name=='nt':awake=bool(ctypes.windll.kernel32.SetThreadExecutionState(0x80000001))
        state['system_sleep_inhibited']=awake
        write(root/'batch_status.json',state)
        # Freeze builds; reject running a queue against a later changed executable.
        def launch(entry,output=None,validate=False):
            nonlocal child
            target=Path(output or entry['output'])
            if target.exists():raise FileExistsError('Existing case is not overwritten: '+str(target))
            if shutil.disk_usage(root).free<20*1024**3:raise RuntimeError('Less than 20 GiB disk free; queue stopped before next case')
            state.update(current_case=entry['case_id'],current_output=str(target),updated=now())
            log=root/'logs'/(entry['case_id']+('_preflight' if validate else '')+'.log')
            command=[str(EXE),'--config',entry['config'],'--output',str(target),'--backend','cuda']+(['--validate'] if validate else [])
            with log.open('w',encoding='utf-8') as stream:
                child=subprocess.Popen(command,cwd=WORK,stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
                state['child_pid']=child.pid;write(root/'batch_status.json',state)
                while child.poll() is None:
                    status=target/'status.json'
                    if status.is_file():
                        try:state['current_progress']=json.loads(status.read_text(encoding='utf-8'))
                        except (OSError,json.JSONDecodeError):pass
                    state['updated']=now();write(root/'batch_status.json',state);time.sleep(2)
                code=child.returncode
            child=None;state.pop('child_pid',None)
            if code or not (target/('READY.txt' if validate else 'COMPLETE.txt')).is_file():raise RuntimeError(f'{entry["case_id"]} failed ({code}); see {log}')
            if not validate:
                write(target/'sha256.json',{str(f.relative_to(target)):digest(f) for f in target.rglob('*') if f.is_file() and f.name!='sha256.json'})
            return target
        seen=set()
        for entry in plan['cases']:
            key=(entry['terrain'],entry['state'],entry['h'] if 'h' in entry else .5,entry.get('depth',200),entry.get('uniform',False))
            if key in seen:continue
            seen.add(key);launch(entry,root/'preflight'/entry['case_id'],True)
        launch(dict(case_id='PREFLIGHT_ASYM_Q25',config=plan['asym_preflight']),root/'preflight/PREFLIGHT_ASYM_Q25',True)
        state['state']='uniform_rayleigh_qualification';write(root/'batch_status.json',state)
        gate_done=False;full_done=False
        for entry in plan['cases']:
            if not entry['case_id'].startswith('G_U_') and not gate_done:
                uniform_gate(root);gate_done=True
            if entry['case_id'].endswith('_R') and not full_done:
                full_domain_gate(root);full_done=True
            state['state']='scientific_pilot' if '/pilot/' in Path(entry['output']).as_posix() else 'numerical_qualification'
            launch(entry)
            if '/pilot/' in Path(entry['output']).as_posix():state['pilot_complete']+=1
            else:state['qualification_complete']+=1
            state['updated']=now();write(root/'batch_status.json',state)
            print(now(),entry['case_id'],'complete',flush=True)
        state.update(state='pilot_data_complete',finished=now());write(root/'batch_status.json',state)
        (root/'PILOT_DATA_COMPLETE.txt').write_text('14 full-duration pilot cases completed after the documented numerical gates. Interpretation and publication validation remain separate.\n',encoding='utf-8')
    except BaseException as error:
        if child is not None and child.poll() is None:child.terminate();child.wait()
        state.update(state='stopped_on_failure',error=str(error),updated=now());write(root/'batch_status.json',state);raise
    finally:
        if awake:ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
        lock.unlink(missing_ok=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--prepare',action='store_true');p.add_argument('--run',action='store_true')
    args=p.parse_args();root=under_out(args.root)
    if args.prepare:prepare(root)
    if args.run:run(root)
