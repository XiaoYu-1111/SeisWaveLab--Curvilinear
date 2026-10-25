"""Independent rectangular DEM region selection and square-spacing export."""
from __future__ import annotations

import html
import ipywidgets as widgets
from IPython.display import clear_output, display
from matplotlib.figure import Figure
from matplotlib.widgets import RectangleSelector

from dem_profiles import export_surface_grid, plot_map, surface_grid_geometry, surface_grid_stem, _dem_detail_colorbar


class DEMRegionSelector:
    """Owns its geometry and canvas; never reads a profile selector's state."""

    def __init__(self, dem, output_dir=None, enable_mouse=True):
        self.dem, self.output_dir = dem, output_dir or dem.output_dir
        self.factor = dem.xy_factor
        if self.factor is None:
            raise ValueError('二维区域导出需先确认 DEM 的平面长度单位。')
        transform = dem.source.transform
        if abs(transform.b)>1e-12 or abs(transform.d)>1e-12:
            raise ValueError('二维区域选择暂不支持旋转栅格，请先重投影为无旋转米制栅格。')
        xa,ya = transform*(.5,.5)
        xb,yb = transform*(dem.source.width-.5,dem.source.height-.5)
        self.left,self.right = sorted((xa,xb))
        self.bottom,self.top = sorted((ya,yb))
        self.total_x = (self.right-self.left)*self.factor
        self.total_y = (self.top-self.bottom)*self.factor
        self._busy = self._closed = self._exporting = False
        self.state = {}
        self.canvas = self.manager = self.figure = self.axis = self.rectangle_selector = self._patch = None
        style = {'description_width':'105px'}
        layout = widgets.Layout(width='100%',min_width='0')
        def number(value,maximum,label):
            return widgets.BoundedFloatText(value=value,min=0,max=maximum,description=label,style=style,layout=layout)
        width,height = min(1000,self.total_x*.8),min(1000,self.total_y*.8)
        self.x_start = number((self.total_x-width)/2,self.total_x,'X 起点偏移 (m)')
        self.y_start = number((self.total_y-height)/2,self.total_y,'Y 起点偏移 (m)')
        self.width = number(width,self.total_x,'X 宽度 (m)')
        self.height = number(height,self.total_y,'Y 长度 (m)')
        self.sampling_mode = widgets.Dropdown(options=[('按 X 点数，自动计算 Y 点数','count'),('按采样间距','spacing')],
            value='count',description='采样方式',style=style,layout=layout)
        self.nx = widgets.BoundedIntText(value=300,min=2,max=4096,description='X 采样点数',style=style,layout=layout)
        self.spacing = widgets.BoundedFloatText(value=2,min=.0001,max=1e8,description='X/Y 间距 (m)',style=style,layout=layout)
        self.nx.layout=widgets.Layout(width='100%',min_width='0')
        self.spacing.layout=widgets.Layout(width='100%',min_width='0')
        self.endpoint = widgets.Dropdown(options=[('不含末端：L/N','exclude'),('包含末端：L/(N−1)','include')],
            value='exclude',description='端点规则',style=style,layout=layout)
        self.interpolation = widgets.Dropdown(options=[('双线性','bilinear'),('最近邻','nearest')],
            value='bilinear',description='高程采样',style=style,layout=layout)
        self.export_format=widgets.Dropdown(options=[('VTI + VTS','both'),('VTI 规则高程数组','vti'),('VTS 起伏地表','vts')],
            value='both',description='导出格式',style=style,layout=layout)
        self.summary,self.status = widgets.HTML(),widgets.HTML()
        self.export_button = widgets.Button(description='导出二维区域',button_style='success')
        self.center_button = widgets.Button(description='居中 1 km × 1 km')
        self.pick_button = widgets.Button(description='恢复框选')
        self.zoom_button = widgets.Button(description='放大到选区')
        self.full_button = widgets.Button(description='查看全 DEM')
        self.rebuild_button = widgets.Button(description='重建交互地图')
        self.map_output,self.export_output = widgets.Output(),widgets.Output()
        def row(items):
            return widgets.Box(items,layout=widgets.Layout(display='flex',flex_flow='row wrap',gap='4px'))
        for button in (self.export_button,self.center_button,self.pick_button,self.zoom_button,self.full_button,self.rebuild_button):
            button.layout=widgets.Layout(width='auto',height='28px')
        controls=widgets.VBox([
            widgets.HTML('<b>二维区域 · 独立选区</b><br><small>偏移从 DEM 左下像元中心计；拖框后可精确修改长宽。</small>'),
            self.x_start,self.y_start,self.width,self.height,self.center_button,
            self.sampling_mode,self.nx,self.spacing,self.endpoint,self.interpolation,self.export_format,
            self.summary,self.export_button],layout=widgets.Layout(flex='1 1 330px',min_width='0'))
        map_panel=widgets.VBox([row([self.pick_button,self.zoom_button,self.full_button,self.rebuild_button]),self.status,self.map_output],
            layout=widgets.Layout(flex='2 1 520px',min_width='0'))
        self.widget=widgets.VBox([row([controls,map_panel]),self.export_output],layout=widgets.Layout(width='100%'))
        for item in (self.x_start,self.y_start,self.width,self.height,self.sampling_mode,self.nx,self.spacing,self.endpoint,self.interpolation,self.export_format):
            item.observe(self._changed,names='value')
        self.export_button.on_click(self.export)
        self.center_button.on_click(self.center_region)
        self.pick_button.on_click(self.restore_picking)
        self.zoom_button.on_click(lambda _:self.show_region())
        self.full_button.on_click(lambda _:self.show_region(full=True))
        self.rebuild_button.on_click(lambda _:self.enable_mouse(force=True))
        self._changed()
        if enable_mouse:self.enable_mouse()

    @property
    def roi(self):
        x=self.left+self.x_start.value/self.factor
        y=self.bottom+self.y_start.value/self.factor
        return (x,x+self.width.value/self.factor,y,y+self.height.value/self.factor)

    def sampling_options(self):
        return dict(nx=self.nx.value,ny=None,
            spacing_m=self.spacing.value if self.sampling_mode.value=='spacing' else None,
            endpoint=self.endpoint.value=='include',method=self.interpolation.value)

    def geometry(self):
        options=self.sampling_options()
        options.pop('method')
        return surface_grid_geometry(self.dem,self.roi,**options)

    def _changed(self,change=None):
        if self._busy or self._closed:return
        self.state.pop('export_folder',None)
        self._busy=True
        try:
            # Numeric inputs keep the requested dimensions. Move the origin only
            # if needed; do not change width/height via interdependent traits.
            self.x_start.value=min(self.x_start.value,max(0,self.total_x-self.width.value))
            self.y_start.value=min(self.y_start.value,max(0,self.total_y-self.height.value))
            self.nx.layout.display='' if self.sampling_mode.value=='count' else 'none'
            self.spacing.layout.display='' if self.sampling_mode.value=='spacing' else 'none'
            try:
                g=self.geometry()
                self.summary.value=(f'<b>{g["nx"]} × {g["ny"]} 点</b>；共 {g["nx"]*g["ny"]:,} 点<br>'
                    f'间距 <b>{g["spacing_m"]:.9g} m</b>（X/Y 相同）<br>'
                    f'选区：{self.width.value:.3f} × {self.height.value:.3f} m<br>'
                    f'节点跨度：{g["width_m"]:.3f} × {g["height_m"]:.3f} m<br>'
                    f'<small>Y 点数按长度与同一间距计算；固定间距不能整除时不追加短间隔。<br>'
                    f'源 DEM 间距：{abs(self.dem.source.transform.a)*self.factor:.6g} × '
                    f'{abs(self.dem.source.transform.e)*self.factor:.6g} m；加密不提高实测分辨率。</small><br>'
                    f'<small>文件名：{surface_grid_stem(g)} .vti / .vts</small>')
                self.export_button.disabled=self._exporting
            except (ValueError,OverflowError) as exc:
                self.summary.value='<b style="color:#b32">'+html.escape(str(exc))+'</b>'
                self.export_button.disabled=True
            self._update_overlay()
        finally:self._busy=False

    def set_roi(self,xmin,xmax,ymin,ymax):
        xa,xb=sorted((float(xmin),float(xmax)))
        ya,yb=sorted((float(ymin),float(ymax)))
        xa,xb=max(self.left,xa),min(self.right,xb)
        ya,yb=max(self.bottom,ya),min(self.top,yb)
        if xa>=xb or ya>=yb:raise ValueError('选区必须与 DEM 相交且长宽为正。')
        self._busy=True
        try:
            self.width.value=(xb-xa)*self.factor
            self.height.value=(yb-ya)*self.factor
            self.x_start.value=(xa-self.left)*self.factor
            self.y_start.value=(ya-self.bottom)*self.factor
        finally:self._busy=False
        self._changed()

    def center_region(self,_=None):
        cx,cy=(self.left+self.right)/2,(self.bottom+self.top)/2
        hx,hy=min(500,self.total_x*.4)/self.factor,min(500,self.total_y*.4)/self.factor
        self.set_roi(cx-hx,cx+hx,cy-hy,cy+hy)

    def _on_rectangle(self,start,end):
        if self._closed or self._exporting or start.inaxes is not self.axis or end.inaxes is not self.axis:return
        if self.canvas.toolbar and self.canvas.toolbar.mode:return
        if None in (start.xdata,start.ydata,end.xdata,end.ydata):return
        try:
            self.set_roi(start.xdata,end.xdata,start.ydata,end.ydata)
            self.status.value='框选已更新。可继续输入精确长宽和采样设置。'
        except ValueError as exc:self.status.value=html.escape(str(exc))

    def _update_overlay(self):
        if self.axis is None:return
        from matplotlib.patches import Rectangle
        if self._patch is not None:self._patch.remove()
        a,b,c,d=self.roi
        self._patch=Rectangle((a,c),b-a,d-c,fill=False,edgecolor='#222',linewidth=.8,zorder=6)
        self.axis.add_patch(self._patch)
        if self.rectangle_selector is not None:
            self.rectangle_selector.extents=self.roi
            self.rectangle_selector.set_visible(True)
        self.canvas.draw_idle()

    def restore_picking(self,_=None):
        if self.canvas is None:
            return self.enable_mouse()
        toolbar=self.canvas.toolbar
        if toolbar is not None:
            if toolbar.mode=='pan/zoom':toolbar.pan()
            elif toolbar.mode=='zoom rect':toolbar.zoom()
        self.rectangle_selector.set_active(True)
        self.status.value='拖动鼠标框选二维区域；拖动边框可调整范围。'

    def show_region(self,full=False):
        if self.axis is None:return
        bounds=(self.left,self.right,self.bottom,self.top) if full else self.roi
        self.axis.set_xlim(bounds[:2]);self.axis.set_ylim(bounds[2:])
        self.restore_picking()
        self.canvas.draw_idle()

    def enable_mouse(self,force=False):
        if self._closed:return False
        if self.canvas is not None and not force:return True
        view=(*self.axis.get_xlim(),*self.axis.get_ylim()) if self.axis is not None else None
        self._close_canvas()
        try:
            from ipympl.backend_nbagg import Canvas,FigureManager
            self.figure=Figure(figsize=(7,4.4),layout='constrained')
            self.canvas=Canvas(self.figure)
            self.manager=FigureManager(self.canvas,id(self))
            self.axis=self.figure.subplots()
            image=plot_map(self.dem,self.axis,max_size=2000)
            _dem_detail_colorbar(self.figure,image,self.axis)
            self.axis.set_title('DEM region selection',fontsize=9)
            self.axis.set_xlim(view[:2] if view else (self.left,self.right))
            self.axis.set_ylim(view[2:] if view else (self.bottom,self.top))
            self.rectangle_selector=RectangleSelector(self.axis,self._on_rectangle,useblit=False,button=[1],
                interactive=True,minspanx=5,minspany=5,spancoords='pixels')
            self._update_overlay()
            self.canvas.header_visible=False
            self.canvas.toolbar_position='top'
            self.canvas.layout.width='100%';self.canvas.layout.height='420px'
            with self.map_output:
                clear_output(wait=True);display(self.canvas)
            self.restore_picking()
            return True
        except Exception as exc:
            self._close_canvas()
            self.status.value='交互地图未启用：'+html.escape(str(exc))+'；可用数值输入。安装 ipympl/ipywidgets、重启内核后重建地图。'
            return False

    def export(self,_=None):
        if self._exporting or self._closed:return None
        self.state.pop('export_folder',None)
        self._exporting=True
        self.export_button.disabled=True
        with self.export_output:
            clear_output(wait=True)
            try:
                formats=('vti','vts') if self.export_format.value=='both' else (self.export_format.value,)
                folder=export_surface_grid(self.dem,self.roi,self.output_dir,**self.sampling_options(),formats=formats)
                self.state['export_folder']=folder
                print('二维区域结果：',folder)
                for suffix in formats:
                    print('三维导入：',folder/(surface_grid_stem(self.geometry())+'.'+suffix))
                print('矩阵 CSV、元数据 JSON、jet 区域图 PNG/PDF/SVG 均在同一文件夹。')
                return folder
            except Exception as exc:
                print(type(exc).__name__+': '+str(exc))
                return None
            finally:
                self._exporting=False
                self._changed_after_export()

    def _changed_after_export(self):
        try:self.geometry();self.export_button.disabled=False
        except (ValueError,OverflowError):self.export_button.disabled=True

    def _close_canvas(self):
        if self.rectangle_selector is not None:self.rectangle_selector.disconnect_events()
        if self.canvas is not None:
            self.canvas.close()
            if self.canvas.toolbar is not None:self.canvas.toolbar.close()
        self.canvas=self.figure=self.axis=self.manager=self.rectangle_selector=self._patch=None

    def close(self):
        self._closed=True
        self._close_canvas()

