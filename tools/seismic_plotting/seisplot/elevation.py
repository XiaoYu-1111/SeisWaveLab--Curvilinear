"""Reproducible single-column elevation-profile figure from an explicit CSV."""
import csv
from pathlib import Path
import numpy as np
from matplotlib import pyplot as plt

def elevation_figure(csv_file, datum_m, source_x_m):
    with Path(csv_file).open(encoding='utf-8-sig',newline='') as stream:
        rows=list(csv.DictReader(stream))
    if len(rows)<2 or not {'x_m','elevation_m'}<=set(rows[0]):
        raise ValueError('Profile CSV requires at least two rows and columns x_m,elevation_m')
    x=np.array([float(r['x_m']) for r in rows]); z=np.array([float(r['elevation_m']) for r in rows])
    if not np.all(np.isfinite(x)) or not np.all(np.isfinite(z)) or np.any(np.diff(x)<=0):
        raise ValueError('Profile coordinates must be finite and strictly increasing')
    if not np.isfinite(datum_m) or not np.isfinite(source_x_m) or not x[0]<=source_x_m<=x[-1]:
        raise ValueError('Datum must be finite and source_x_m must lie within the profile')
    source_z=float(np.interp(source_x_m,x,z))
    fig,ax=plt.subplots(figsize=(3.21,1.8),layout='constrained')
    ax.plot(x/1000,z,color='#1f4f67',lw=1.2,label='Surface elevation')
    ax.axhline(datum_m,color='#555555',ls='--',lw=.8,label='Reference datum')
    ax.plot([source_x_m/1000],[source_z],marker='*',color='#b2312f',ms=6,label='Source projection')
    ax.set(xlabel='Horizontal distance / km',ylabel='Elevation / m',xlim=(x[0]/1000,x[-1]/1000))
    ax.grid(alpha=.2,lw=.5)
    ax.legend(loc='upper left',frameon=True,framealpha=.85,fontsize=7,handlelength=1.5,borderpad=.3,labelspacing=.1)
    return fig,{'samples':len(x),'x_min_m':float(x[0]),'x_max_m':float(x[-1]),'source_x_m':float(source_x_m),
                'source_elevation_m':source_z,'datum_m':float(datum_m),'profile_min_m':float(z.min()),'profile_max_m':float(z.max())}
