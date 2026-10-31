"""Display regressions: preserve SI data, physical coordinates and common scales."""
import sys,unittest
from pathlib import Path
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import seisplot
import numpy as np
from matplotlib import pyplot as plt
from seisplot.publication import resolve_style,reference_lines,ordered_components,prepare_journal_figure
from seisplot.core import gain,gather_image,symlog
from seisplot.io import Gather


class PublicationTests(unittest.TestCase):
    def tearDown(self):plt.close('all')

    def test_shared_frequency_scale_and_override_without_mutation(self):
        config={'style_profile':'jingbian_15_30','records':{'agc_clip':2.5}}
        resolved=resolve_style(config)
        self.assertEqual(config,{'style_profile':'jingbian_15_30','records':{'agc_clip':2.5}})
        self.assertEqual(len(resolved['snapshots']['times_s']),9)
        self.assertEqual(resolved['snapshots']['vmax_m_s'],2e-5)
        self.assertEqual(resolved['snapshots']['linthresh_m_s'],2e-7)
        self.assertEqual(resolved['records']['agc_clip'],2.5)
        self.assertEqual(resolve_style(resolved),resolved)
        self.assertEqual(ordered_components(['Vx','Vz']),['Vz','Vx'])

    def test_raw_micrometers_and_agc_dimensionless(self):
        source=np.array([[-1e-6,0,1e-6],[-2e-6,0,2e-6]])
        before=source.copy();g=Gather(source,.1,np.array([0.,1000.]),np.zeros(2),np.array([-500.,500.]))
        cfg=resolve_style({'style_profile':'jingbian_15_30'})
        _,ax=plt.subplots();im=gather_image(ax,g,source,'',cfg,2e-6)
        np.testing.assert_array_equal(im.get_array(),source.T*1e6)
        self.assertEqual(im.get_clim(),(-2.,2.))
        np.testing.assert_allclose(im.get_cmap()(np.array([0.,.5,1.]))[:,:3],[[0,0,0],[.502,.502,.502],[1,1,1]],atol=.002)
        self.assertEqual(im.get_interpolation(),'bilinear')
        self.assertEqual(im.get_interpolation_stage(),'data')
        self.assertEqual(ax.get_xlabel(),'接收点 X / km')
        _,ax=plt.subplots();im=gather_image(ax,g,source,'',cfg,3.,mode='agc')
        np.testing.assert_array_equal(im.get_array(),source.T)
        self.assertEqual(im.get_clim(),(-3.,3.))
        np.testing.assert_array_equal(source,before)

    def test_agc_truncated_edges_and_floor(self):
        source=np.array([[1.,2.,3.,4.,5.],[1e-6]*5]);before=source.copy()
        local=np.sqrt(np.array([[2.5,14/3,29/3,50/3,20.5],[1e-12]*5]))
        expected=source/np.maximum(local,.1)
        np.testing.assert_allclose(gain(source,1.,'agc',3.,.01,'truncated',False,10.),expected)
        np.testing.assert_array_equal(source,before)

    def test_interfaces_only_in_physical_coordinates(self):
        _,ax=plt.subplots();ax.set_ylim(4000,-300)
        artists=reference_lines(ax,[1000,2000,5000],{'depth_coordinate':'physical'})
        self.assertEqual(len(artists),4)
        self.assertEqual([t.get_text() for t in ax.texts],['1000 m','2000 m'])
        self.assertEqual(reference_lines(ax,[1000],{'depth_coordinate':'eta'}),[])
        self.assertEqual(reference_lines(ax,[1000],{'depth_coordinate':'surface_relative'}),[])

    def test_invalid_display_configuration_rejected(self):
        for key,value in [('agc_window_s',0),('agc_clip',-3),('agc_floor',-1)]:
            with self.assertRaises(ValueError):resolve_style({'records':{key:value}})
        with self.assertRaises(ValueError):resolve_style({'snapshots':{'scale_scope':'fixed'}})
        norm=symlog(2e-5,.01)
        np.testing.assert_allclose(norm([-2e-5,0,2e-5]),[0,.5,1])

    def test_journal_layout_keeps_colormap_norm_and_data(self):
        cfg=resolve_style({'style_profile':'jingbian_15_30','journal_layout':'ogp_single'})
        self.assertEqual(resolve_style(cfg),cfg)
        self.assertEqual(cfg['record_cmap'],'gray')
        self.assertEqual(cfg['models']['cmap'],'turbo')
        self.assertEqual(cfg['snapshots']['cmap'],'coolwarm')
        self.assertEqual(cfg['snapshots']['vmax_m_s'],2e-5)
        data=np.array([[-2.,0,2.],[-1.,0,1.]])
        fig,ax=plt.subplots(figsize=(7,4),layout='constrained')
        im=ax.imshow(data,cmap=cfg['snapshots']['cmap'],vmin=-2,vmax=2)
        self.assertTrue(prepare_journal_figure(fig,cfg))
        self.assertAlmostEqual(fig.get_figwidth()*25.4,81.5)
        self.assertEqual(im.get_clim(),(-2,2))
        self.assertEqual(im.get_cmap().name,'coolwarm')
        np.testing.assert_array_equal(im.get_array(),data)
        with self.assertRaises(ValueError):resolve_style({'journal_layout':'not-a-layout'})


if __name__=='__main__':unittest.main()
