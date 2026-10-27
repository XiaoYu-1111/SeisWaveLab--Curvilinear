"""Compatibility for historical out/tools names; all rendering uses the maintained package."""
import argparse,json,sys
from pathlib import Path
sys.dont_write_bytecode=True
from seisplot.core import load_config
from run import run


def main(frequency=None,steps='records',individual=False):
    p=argparse.ArgumentParser(description='旧入口转发至统一绘图工具；新图写入外层out/replots，不覆盖正式PPT。')
    p.add_argument('--frequency',type=int,choices=[15,30],default=frequency)
    p.add_argument('--steps',default=steps)
    p.add_argument('--check-config',action='store_true')
    a=p.parse_args()
    cfg=load_config(Path(__file__).parent/'examples/four_frequency.json')
    if a.frequency:
        cfg['datasets']=[d for d in cfg['datasets'] if d['label']==f'{a.frequency}Hz']
        cfg['output_dir']=str(Path(cfg['output_dir'])/f'{a.frequency}Hz')
    cfg['snapshots']['individual']=individual
    print('统一绘图入口：',Path(__file__).parent,flush=True)
    print('输出目录：',cfg['output_dir'],flush=True)
    if a.check_config:
        print(json.dumps(cfg,ensure_ascii=False,indent=2));return
    run('four',cfg,a.steps.split(','))


if __name__=='__main__':main()
