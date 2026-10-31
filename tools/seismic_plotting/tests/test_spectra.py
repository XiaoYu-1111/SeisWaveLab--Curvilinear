import unittest
import sys
from pathlib import Path
import numpy as np
import csv,json,tempfile
from types import SimpleNamespace
from unittest.mock import patch
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from seisplot.spectra import trace_amplitude_spectra
from seisplot.four_case import FourCaseStudy
from seisplot.core import WORKSPACE


class TraceSpectraTests(unittest.TestCase):
    def test_opposite_phase_traces_average_amplitudes_not_waveforms(self):
        dt=.001;t=np.arange(4096)*dt;a=np.sin(2*np.pi*31.25*t)
        f,p,mean=trace_amplitude_spectra(np.array([a,-2*a]),dt)
        np.testing.assert_allclose(p[1],2*p[0],rtol=1e-12,atol=1e-25)
        np.testing.assert_allclose(mean,1.5*p[0],rtol=1e-12,atol=1e-25)
        self.assertAlmostEqual(f[mean.argmax()],31.25)
        self.assertAlmostEqual(mean.max(),1.5,places=12)

    def test_short_zero_records_and_invalid_inputs(self):
        f,p,mean=trace_amplitude_spectra(np.zeros((3,16)),.01)
        self.assertEqual(p.shape,(3,9))
        np.testing.assert_array_equal(mean,np.zeros(9))
        for a,dt in [(np.zeros(10),.01),(np.zeros((2,10)),0),
                       (np.zeros((2,1)),.01),(np.full((1,10),np.nan),.01)]:
            with self.assertRaises(ValueError):trace_amplitude_spectra(a,dt)

    def test_nyquist_and_odd_trace_normalization(self):
        a=(-1.)**np.arange(64)
        f,p,_=trace_amplitude_spectra(a[None,:],.001,'boxcar')
        self.assertAlmostEqual(p[0,-1],1.)
        n=63;t=np.arange(n);a=3*np.cos(2*np.pi*5*t/n)
        f,p,_=trace_amplitude_spectra(a[None,:],.001,'boxcar')
        self.assertAlmostEqual(p[0,5],3.)

    def test_display_band_does_not_truncate_saved_spectrum(self):
        # A 250 Hz component must survive in the CSV even though the figure
        # and its panels JSON stop at 100 Hz.
        t=np.arange(1000)*.001
        g=SimpleNamespace(values=np.array([np.sin(2*np.pi*20*t)+.5*np.sin(2*np.pi*250*t)]),
                          dt=.001,x=np.array([10.]),offset=np.array([0.]))
        case={'case':{'id':'M00'},'components':{'Vz':'fixture.sgy'}}
        root=WORKSPACE/'out/validation/seismic_plotting';root.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as folder:
            out=Path(folder);(out/'data').mkdir();(out/'figures').mkdir()
            study=object.__new__(FourCaseStudy);study.out=out
            study.cfg={'trace_spectra':{'display_max_hz':100},'formats':['png']}
            study.datasets=[{'label':'test','cases':[case]}];study.get=lambda *args:g
            def inspect_figure(fig,*args):
                self.assertEqual(fig.axes[0].get_xlim(),(0,100))
                from matplotlib import pyplot as plt
                plt.close(fig);return []
            with patch('seisplot.four_case.save',side_effect=inspect_figure):study.plot_trace_spectra()
            with (out/'data/test_M00_Vz_trace_spectra.csv').open(encoding='utf-8-sig',newline='') as f:rows=list(csv.DictReader(f))
            self.assertEqual(float(rows[-1]['frequency_hz']),500.)
            self.assertAlmostEqual(float(rows[250]['mean_amplitude_m_s']),.5,places=10)
            panel=json.loads((out/'data/trace_spectra_panels.json').read_text(encoding='utf8'))[0]
            self.assertEqual(panel['frequency_hz'][-1],100.)
            self.assertEqual(panel['data_max_hz'],500.)
