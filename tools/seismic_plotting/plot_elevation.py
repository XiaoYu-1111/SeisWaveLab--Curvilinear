"""Render an elevation profile without altering the source data or other figures."""
import argparse,hashlib,json,sys
from pathlib import Path
sys.dont_write_bytecode=True
from seisplot.core import setup,save
from seisplot.publication import resolve_style
from seisplot.elevation import elevation_figure

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile',required=True,type=Path)
    p.add_argument('--datum-m',required=True,type=float)
    p.add_argument('--source-x-m',required=True,type=float)
    p.add_argument('--output-dir',required=True,help='Subdirectory of the workspace out directory')
    args=p.parse_args()
    cfg=resolve_style({'output_dir':args.output_dir,'journal_layout':'ogp_single','formats':['png','pdf','svg']})
    out=setup(cfg)
    fig,meta=elevation_figure(args.profile,args.datum_m,args.source_x_m)
    meta['profile_file']=str(args.profile.resolve());meta['profile_sha256']=hashlib.sha256(args.profile.read_bytes()).hexdigest()
    (out/'data/elevation_profile_provenance.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2),encoding='utf-8')
    for output in save(fig,out,'elevation_profile_single_column',cfg):print(output)

if __name__=='__main__':main()
