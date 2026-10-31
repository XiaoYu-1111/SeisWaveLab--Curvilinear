"""Run the 2D CPU32 / GPU FP32 / FP64 matrix with live NVML telemetry.

No solver changes; power settings are queried, attempted within supported limits,
verified and restored in finally. All output must stay in the outer out folder.
"""
from pathlib import Path
import argparse
import csv
import ctypes as C
import hashlib
import itertools
import json
import os
import re
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent/'out'
EXE = OUT/'bin/Release/seis2d_precision_benchmark.exe'

def command(args):
    result = subprocess.run(args, capture_output=True)
    def decode(value):
        try:return value.decode('utf-8')
        except UnicodeDecodeError:return value.decode('mbcs',errors='replace')
    return {'command': args, 'returncode': result.returncode,
            'stdout': decode(result.stdout), 'stderr': decode(result.stderr)}

class Util(C.Structure):
    _fields_ = [('gpu', C.c_uint), ('memory', C.c_uint)]

class Nvml:
    def __init__(self):
        self.dll = C.WinDLL('nvml.dll')
        assert self.dll.nvmlInit_v2() == 0, 'NVML initialization failed'
        self.handle = C.c_void_p()
        assert self.dll.nvmlDeviceGetHandleByIndex_v2(0, C.byref(self.handle)) == 0

    def scalar(self, name, *args, kind=C.c_uint):
        value = kind()
        return value.value if getattr(self.dll, name)(self.handle, *args, C.byref(value)) == 0 else None

    def limits(self):
        low, high = C.c_uint(), C.c_uint()
        rc = self.dll.nvmlDeviceGetPowerManagementLimitConstraints(self.handle, C.byref(low), C.byref(high))
        enforced=self.scalar('nvmlDeviceGetEnforcedPowerLimit')
        return {'min_w': low.value/1000 if rc == 0 else None, 'max_w': high.value/1000 if rc == 0 else None,
                'enforced_w': enforced/1000 if enforced is not None else None}

    def sample(self):
        util = Util()
        rc = self.dll.nvmlDeviceGetUtilizationRates(self.handle, C.byref(util))
        power = self.scalar('nvmlDeviceGetPowerUsage')
        limit = self.scalar('nvmlDeviceGetEnforcedPowerLimit')
        return {'epoch': time.time(), 'power_w': power/1000 if power is not None else None,
                'enforced_limit_w': limit/1000 if limit is not None else None,
                'gpu_util_percent': util.gpu if rc == 0 else None,
                'memory_util_percent': util.memory if rc == 0 else None,
                'sm_mhz': self.scalar('nvmlDeviceGetClockInfo', 1),
                'memory_mhz': self.scalar('nvmlDeviceGetClockInfo', 2),
                'temperature_c': self.scalar('nvmlDeviceGetTemperature', 0),
                'pstate': self.scalar('nvmlDeviceGetPerformanceState'),
                'clock_event_mask': self.scalar('nvmlDeviceGetCurrentClocksThrottleReasons', kind=C.c_ulonglong)}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT/'benchmarks/precision_cpu32_gpu_20261029')
    parser.add_argument('--sizes', type=int, nargs='+', default=[256, 1024, 1536, 2048])
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    dest = args.output.resolve()
    assert dest.is_relative_to(OUT.resolve()) and dest != OUT.resolve()
    assert args.repeats >= 3 and args.repeats % 2 == 1 and all(n in [256, 1024, 1536, 2048] for n in args.sizes)
    dest.mkdir(parents=True, exist_ok=True)
    assert not (dest/'manifest.json').exists() or args.resume, 'Use --resume for an existing run'
    if args.resume and (dest/'COMPLETE.txt').is_file():
        print('Already complete; original measurements and power state preserved.',flush=True);return
    previous=json.loads((dest/'manifest.json').read_text(encoding='utf-8')) if args.resume and (dest/'manifest.json').is_file() else None
    settings = {256: (10, 100), 1024: (5, 40), 1536: (3, 20), 2048: (3, 20)}
    manifest = {'date': '2026-10-29', 'sizes': args.sizes, 'repeats': args.repeats, 'cpu_threads': 32,
                'warmup_timed_steps': settings, 'preheat_ms': 1000, 'telemetry_interval_s': .1,
                'executable_sha256': hashlib.sha256(EXE.read_bytes()).hexdigest(),
                'benchmark_source_sha256': hashlib.sha256((ROOT/'benchmarks/PrecisionBenchmark2D.cpp').read_bytes()).hexdigest(),
                'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'started_epoch': time.time(), 'pid': os.getpid(), 'status': 'running'}
    if previous:
        for key in ['executable_sha256','benchmark_source_sha256','sizes','repeats']:
            assert manifest[key]==previous[key], f'Resume configuration differs: {key}'
        manifest['started_epoch']=previous['started_epoch']
        manifest['previous_segments']=previous.get('previous_segments',[])+[previous['power_control']]
    control = {'before_plan': command(['powercfg', '/getactivescheme']),
               'before_gpu': command(['nvidia-smi', '-q', '-x'])}
    old_guid = re.search(r'[0-9a-fA-F-]{36}', control['before_plan']['stdout']).group()
    nvml = Nvml()
    control['before_limits'] = nvml.limits()
    manifest['power_control'] = control
    (dest/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    stop = threading.Event()
    telemetry_error = []
    def monitor():
        with (dest/'telemetry.csv').open('a', newline='', encoding='utf-8') as f:
            writer = None
            while not stop.is_set():
                try:
                    row = nvml.sample()
                    if writer is None:
                        writer = csv.DictWriter(f, fieldnames=list(row))
                        if f.tell() == 0: writer.writeheader()
                    writer.writerow(row);f.flush()
                except Exception as error:
                    telemetry_error.append(repr(error));break
                stop.wait(.1)
    thread = threading.Thread(target=monitor, daemon=True)
    env = os.environ.copy()
    env.update(OMP_NUM_THREADS='32', OMP_DYNAMIC='FALSE', OMP_WAIT_POLICY='PASSIVE')
    try:
        control['set_high_performance'] = command(['powercfg', '/setactive', 'SCHEME_MIN'])
        control['during_plan'] = command(['powercfg', '/getactivescheme'])
        if control['before_limits']['max_w'] is not None:
            control['request_max_gpu_power'] = command(['nvidia-smi', '-i', '0', '-pl', str(control['before_limits']['max_w'])])
        control['verified_limits'] = nvml.limits()
        (dest/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
        print('POWER', json.dumps(control['verified_limits']), control.get('request_max_gpu_power',{}).get('stdout','No supported power setter').strip(), flush=True)
        thread.start()
        modes = [('CPU', 'FP64'), ('GPU', 'FP32'), ('CPU', 'FP32'), ('GPU', 'FP64')]
        trials = []
        for size, case in itertools.product(args.sizes, ['elastic', 'gsls3']):
            warmup, steps = settings[size]
            for repeat in range(args.repeats):
                order = modes[repeat % 4:]+modes[:repeat % 4]
                for backend, precision in order:
                    run_id = f'{size}_{case}_{backend}_{precision}_r{repeat+1}'
                    folder = dest/'trials'/run_id
                    folder.mkdir(parents=True, exist_ok=True)
                    if args.resume and (folder/'status.json').exists():
                        status = json.loads((folder/'status.json').read_text(encoding='utf-8'))
                        trials.append(status);continue
                    print('START', run_id, f'warm={warmup} timed={steps}', flush=True)
                    with (folder/'process.log').open('w', encoding='utf-8') as log:
                        proc = subprocess.run([str(EXE), '--output', str(folder), '--size', str(size), '--case', case,
                                               '--backend', backend, '--precision', precision, '--threads', '32',
                                               '--warmup', str(warmup), '--steps', str(steps), '--preheat-ms', '1000'],
                                              env=env, stdout=log, stderr=subprocess.STDOUT)
                    status = {'run_id': run_id, 'size': size, 'case': case, 'backend': backend,
                              'precision': precision, 'repeat': repeat+1, 'returncode': proc.returncode}
                    text = (folder/'process.log').read_text(encoding='utf-8')
                    if proc.returncode == 0:
                        status.update(json.loads((folder/'trial.json').read_text(encoding='utf-8')))
                        status['status'] = 'OK'
                        print('DONE', run_id, f'{status["step_ms"]:.3f} ms/step', flush=True)
                    elif 'Insufficient RAM' in text or 'Insufficient VRAM' in text:
                        status.update(status='RESOURCE_REJECTED', reason=text.strip())
                        print('REJECTED', run_id, text.strip(), flush=True)
                    else:
                        raise RuntimeError(run_id+' '+text)
                    (folder/'status.json').write_text(json.dumps(status, ensure_ascii=False, indent=2), encoding='utf-8')
                    trials.append(status)
                    (dest/'trials.json').write_text(json.dumps(trials, ensure_ascii=False, indent=2), encoding='utf-8')
        assert not telemetry_error, telemetry_error
        manifest['status'] = 'complete'
        manifest['trial_count'] = len(trials)
        manifest['success_count'] = sum(r['status'] == 'OK' for r in trials)
        manifest['resource_rejected_count'] = sum(r['status'] == 'RESOURCE_REJECTED' for r in trials)
    except BaseException as error:
        manifest['status']='interrupted' if isinstance(error,KeyboardInterrupt) else 'failed'
        manifest['error']=repr(error)
        raise
    finally:
        stop.set()
        if thread.is_alive():thread.join(timeout=5)
        limit = nvml.limits()['enforced_w']
        if limit != control['before_limits']['enforced_w']:
            control['restore_gpu_limit'] = command(['nvidia-smi', '-i', '0', '-pl', str(control['before_limits']['enforced_w'])])
        control['after_limits'] = nvml.limits()
        control['restore_plan'] = command(['powercfg', '/setactive', old_guid])
        control['after_plan'] = command(['powercfg', '/getactivescheme'])
        manifest['finished_epoch'] = time.time()
        manifest['telemetry_errors'] = telemetry_error
        (dest/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
        nvml.dll.nvmlShutdown()
    assert manifest['status'] == 'complete'
    (dest/'COMPLETE.txt').write_text('All requested trials completed or explicitly resource-rejected. Power requests and final observations recorded.\n', encoding='utf-8')
    print('COMPLETE', manifest['success_count'], 'OK;', manifest['resource_rejected_count'], 'resource-rejected', flush=True)

if __name__ == '__main__':
    main()
