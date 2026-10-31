"""Command line companion to the notebooks. No simulation or inversion is run."""
import argparse,sys
from pathlib import Path
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent))
from seisplot.core import load_config
from seisplot.four_case import FourCaseStudy
from seisplot.surface_wave import SurfaceWaveStudy
from seisplot.inversion import InversionStudy
from seisplot.publication import style_catalog

def run(kind,config,steps=None):
    classes={'four':FourCaseStudy,'masw':SurfaceWaveStudy,'inversion':InversionStudy}
    study=classes[kind](config)
    methods={
      'four':{'inspect':'inspect','terrain':'plot_terrain_profile','records':'plot_records','spectra':'plot_trace_spectra','models':'plot_models','wavefields':'plot_wavefields','metrics':'plot_metrics','frequencies':'plot_frequency_compare','reflections':'plot_reflections'},
      'masw':{'inspect':'inspect','records':'plot_records','models':'plot_models','wavefields':'plot_wavefields','differences':'plot_record_differences','traces':'plot_trace_overlay','dispersion':'plot_dispersion','windows':'plot_offset_windows','picks':'plot_phase_curves'},
      'inversion':{'models':'plot_models','fit':'plot_curve_fit','multistart':'plot_multistart','thickness':'plot_thickness_scan','sensitivity':'plot_sensitivity'}}[kind]
    for step in steps or list(methods):
        if step not in methods:raise ValueError(f'Unknown {kind} step: {step}; use {list(methods)}')
        getattr(study,methods[step])();print('Completed:',step,flush=True)
    return study

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kind',choices=['four','masw','inversion'])
    p.add_argument('--config',type=Path)
    p.add_argument('--list-styles',action='store_true',help='Show versioned display profiles')
    p.add_argument('--frequency',type=int,choices=[15,30],help='Select one frequency from the four-model example')
    p.add_argument('--output-dir',help='Override output below the workspace outer out directory')
    p.add_argument('--check-config',action='store_true',help='Print effective configuration without reading simulation arrays')
    p.add_argument('--steps',help='Comma-separated steps; omit to run all')
    a=p.parse_args()
    if a.list_styles:
        import json
        catalog=style_catalog();print(json.dumps({'version':catalog['version'],'profiles':list(catalog['profiles']),'colors':catalog['colors']},ensure_ascii=False,indent=2));raise SystemExit
    if not a.kind or not a.config:p.error('--kind and --config are required unless --list-styles is used')
    config=load_config(a.config)
    if a.frequency:
        if a.kind!='four':p.error('--frequency applies to four-model studies only')
        config['datasets']=[d for d in config['datasets'] if d['label']==f'{a.frequency}Hz']
        if not config['datasets']:p.error('Requested frequency label is absent from the configuration')
        config['output_dir']=str(Path(config['output_dir'])/f'{a.frequency}Hz')
    if a.output_dir:config['output_dir']=a.output_dir
    if a.check_config:
        import json
        print(json.dumps(config,ensure_ascii=False,indent=2));raise SystemExit
    study=run(a.kind,config,a.steps.split(',') if a.steps else None)
    print('Output:',study.out)
