"""One-sided linear amplitude spectra, averaged after the trace FFTs."""
import numpy as np
from scipy import signal


def trace_amplitude_spectra(values, dt, window='hann'):
    values = np.asarray(values, dtype=float)
    if values.ndim != 2 or values.shape[0] < 1 or values.shape[1] < 2:
        raise ValueError('Spectra require at least one trace and two time samples')
    if not np.isfinite(values).all() or not np.isfinite(dt) or dt <= 0:
        raise ValueError('Spectra require finite samples and a positive dt')
    if window not in ['hann','boxcar']:
        raise ValueError('Amplitude spectrum window must be hann or boxcar')
    n=values.shape[1];w=signal.get_window(window,n,fftbins=True)
    centered=values-values.mean(axis=-1,keepdims=True)
    amplitude=np.abs(np.fft.rfft(centered*w,axis=-1))/w.sum()
    amplitude[:,1:]*=2
    if n%2==0:amplitude[:,-1]*=.5
    f=np.fft.rfftfreq(n,dt)
    return f,amplitude,amplitude.mean(axis=0)
