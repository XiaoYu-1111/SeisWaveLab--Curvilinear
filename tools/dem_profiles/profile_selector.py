"""Notebook controls for ROI translation, profile start/length and map picking."""
from __future__ import annotations

import html
import math
import sys

import ipywidgets as widgets
import matplotlib.pyplot as plt
import numpy as np
from IPython.display import clear_output, display

from dem_profiles import extract_profile, export_profile, line_in_roi, longest_valid_segment, plot_map, plot_profile


class DEMProfileSelector:
    def __init__(self, dem, output_dir=None, distance_mode='grid', enable_mouse=True):
        self.dem, self.output_dir, self.distance_mode = dem, output_dir or dem.output_dir, distance_mode
        self.left, self.bottom, self.right, self.top = dem.bounds
        transform=dem.source.transform
        if abs(transform.b)<1e-12 and abs(transform.d)<1e-12:
            x0,y0=transform*(.5,.5)
            x1,y1=transform*(dem.source.width-.5,dem.source.height-.5)
            self.left,self.right=min(x0,x1),max(x0,x1)
            self.bottom,self.top=min(y0,y1),max(y0,y1)
        self.factor = dem.xy_factor or 1.0
        self.unit = 'm' if dem.xy_factor is not None else 'source units'
        self.total_x = (self.right-self.left)*self.factor
        self.total_y = (self.top-self.bottom)*self.factor
        self.epsilon = max(1e-4, min(self.total_x, self.total_y)*1e-9)
        self._busy = False
        self._closed = False
        self._exporting = False
        self._press = None
        self._pending = None
        self.canvas = self.manager = self.figure = self.axis = self.rectangle_selector = None
        self._connections = []
        self.state = {}
        self.full_length = 0.0
        style = {'description_width':'112px'}
        def slider(value, low, high, description):
            return widgets.FloatSlider(value=value,min=low,max=high,step=1 if high>100 else max(high/1000,1e-5),
                                       description=description,readout_format='.2f',continuous_update=False,
                                       style=style,layout=widgets.Layout(width='98%'))
        width, height = min(1000,self.total_x*.8), min(1000,self.total_y*.8)
        self.roi_width = slider(width,self.epsilon,self.total_x,f'区域 X 宽度 ({self.unit})')
        self.roi_height = slider(height,self.epsilon,self.total_y,f'区域 Y 高度 ({self.unit})')
        self.roi_x_start = slider((self.total_x-width)/2,0,self.total_x-width,f'X 起点偏移 ({self.unit})')
        self.roi_y_start = slider((self.total_y-height)/2,0,self.total_y-height,f'Y 起点偏移 ({self.unit})')
        self.roi_x = widgets.FloatRangeSlider(value=(self.left+(self.total_x-width)/2/self.factor,
                                                     self.left+(self.total_x+width)/2/self.factor),
                                              min=self.left,max=self.right,continuous_update=False,
                                              readout_format='.3f',description='DEM X 绝对范围',style=style,
                                              layout=widgets.Layout(width='98%'))
        self.roi_y = widgets.FloatRangeSlider(value=(self.bottom+(self.total_y-height)/2/self.factor,
                                                     self.bottom+(self.total_y+height)/2/self.factor),
                                              min=self.bottom,max=self.top,continuous_update=False,
                                              readout_format='.3f',description='DEM Y 绝对范围',style=style,
                                              layout=widgets.Layout(width='98%'))
        self.direction = widgets.Dropdown(options=[('横向 / X 增大','horizontal'),('纵向 / Y 增大','vertical'),
                                                   ('任意两点','custom')],value='horizontal',description='剖面方向',style=style)
        self.position = widgets.FloatSlider(value=.5,min=0,max=1,continuous_update=False)
        self.cross_position = slider(height/2,0,height,f'剖面 Y 位置 ({self.unit})')
        self.spacing = widgets.BoundedFloatText(value=10,min=.0001,max=1e5,description='采样间距 (m)',style=style)
        self.start_dist = slider(0,0,max(width-self.epsilon,self.epsilon),'截取起点 (m)')
        self.cut_length = slider(min(1000,width),self.epsilon,width,'截取长度 (m)')
        self.end_dist = widgets.BoundedFloatText(value=width,min=0,max=1e12,description='截取终点 (m)',style=style)
        self.interpolation = widgets.Dropdown(options=[('双线性','bilinear'),('最近邻','nearest')],value='bilinear',
                                              description='高程采样',style=style)
        self.trim_invalid = widgets.Checkbox(value=False,description='只保留最长连续有效段',indent=False)
        self.live_preview = widgets.Checkbox(value=True,description='滑条释放后自动更新预览',indent=False)
        self.height_view = widgets.Dropdown(options=[('实际高程','absolute'),('相对起点','start'),
                                                     ('相对样点均值','mean')], value='absolute',
                                             description='曲线高程',style=style)
        self.annotate_extrema = widgets.Checkbox(value=True,description='标注最高 / 最低点',indent=False)
        self.zoom_enabled = widgets.Checkbox(value=False,description='同时预览 / 导出曲线局部图',indent=False)
        self.zoom_start = widgets.BoundedFloatText(value=0,min=0,max=width,description='放大起点 (m)',style=style)
        self.zoom_end = widgets.BoundedFloatText(value=min(200,width),min=0,max=width,description='放大终点 (m)',style=style)
        self.move_step = widgets.BoundedFloatText(value=100,min=.01,max=1e8,description='起点微移步长 (m)',style=style)
        self.previous = widgets.Button(description='起点 ←')
        self.next = widgets.Button(description='起点 →')
        self.whole_line = widgets.Button(description='选取整条线')
        self.center_roi = widgets.Button(description='区域居中 1 km')
        self.full_roi = widgets.Button(description='全 DEM 范围')
        self.x0 = widgets.FloatText(value=self.roi_x.value[0],description='起点 DEM X',style=style)
        self.y0 = widgets.FloatText(value=self.roi_y.value[0],description='起点 DEM Y',style=style)
        self.x1 = widgets.FloatText(value=self.roi_x.value[1],description='终点 DEM X',style=style)
        self.y1 = widgets.FloatText(value=self.roi_y.value[1],description='终点 DEM Y',style=style)
        self.custom_box = widgets.VBox([self.x0,self.y0,self.x1,self.y1])
        self.summary = widgets.HTML()
        self.mouse_status = widgets.HTML()
        self.mouse_mode = widgets.ToggleButtons(options=[('定位起点','locate'),('拖框区域','rectangle'),
                                                         ('两点剖面','endpoints'),('关闭','off')],value='locate',
                                                style={'button_width':'88px'})
        self.connect_button = widgets.Button(description='启用 / 重建交互地图')
        self.pick_button = widgets.Button(description='恢复点选',tooltip='退出平移 / 缩放工具，恢复当前点选方式')
        self.export_scope = widgets.Dropdown(options=[('自动：缩放视口 / 选区','auto'),
                                                       ('当前地图视口','viewport'),('选取矩形','selection')],
                                              value='auto',description='局部 DEM 范围',style=style)
        self.refresh_button = widgets.Button(description='更新剖面',button_style='info')
        self.export_button = widgets.Button(description='导出剖面结果',button_style='success')
        self.dem_export_button = widgets.Button(description='放大到选区',
                                                tooltip='将交互地图放大到橙色矩形，再导出当前地图视口截图')
        self.result_output, self.export_output, self.map_output = widgets.Output(),widgets.Output(),widgets.Output()
        absolute = widgets.Accordion(children=[widgets.VBox([self.roi_x,self.roi_y])],selected_index=None)
        absolute.set_title(0,'精确坐标范围')
        for item in (self.direction,self.spacing,self.end_dist,self.interpolation,self.move_step,
                     self.height_view,self.export_scope,self.zoom_start,self.zoom_end,self.x0,self.y0,self.x1,self.y1):
            item.layout=widgets.Layout(width='100%',min_width='0')
        for button in (self.previous,self.next,self.whole_line,self.center_roi,self.full_roi,
                       self.refresh_button,self.export_button,self.dem_export_button,self.connect_button,self.pick_button):
            button.layout=widgets.Layout(width='auto',min_width='70px',height='28px')
        def row(items):
            return widgets.Box(items,layout=widgets.Layout(display='flex',flex_flow='row wrap',gap='4px'))
        advanced = widgets.Accordion(children=[widgets.VBox([
            absolute,self.end_dist,self.move_step,row([self.previous,self.next,self.whole_line]),
            self.interpolation,self.trim_invalid])],selected_index=None)
        advanced.set_title(0,'精确坐标、终点与微调')
        zoom = widgets.Accordion(children=[widgets.VBox([
            self.zoom_enabled,self.zoom_start,self.zoom_end,
            widgets.HTML('<small>范围按当前导出剖面起点计距；保留全剖面的高程显示基准，不改变 CSV。</small>')])],selected_index=None)
        zoom.set_title(0,'高程曲线局部放大（可选）')
        self.controls = widgets.VBox([
            widgets.HTML('<b>区域与剖面</b> <span style="color:#666">偏移按 DEM 左下像元中心计</span>'),
            self.roi_x_start,self.roi_width,self.roi_y_start,self.roi_height,
            row([self.center_roi,self.full_roi]),self.direction,self.cross_position,
            self.custom_box,self.start_dist,self.cut_length,self.spacing,
            self.height_view,row([self.annotate_extrema,self.live_preview]),self.export_scope,advanced,zoom,
            row([self.refresh_button,self.export_button,self.dem_export_button])],
            layout=widgets.Layout(flex='1 1 410px',min_width='0',padding='0 8px 0 0'))
        self.map_panel = widgets.VBox([
            widgets.HTML('<b>交互地图（在此点选）</b> <span style="color:#666">静态曲线图片不支持点选；导航后点“恢复点选”</span>'),
            row([self.mouse_mode,self.pick_button,self.connect_button]),self.mouse_status,self.map_output],
            layout=widgets.Layout(flex='2 1 520px',min_width='0'))
        self.widget = widgets.VBox([
            widgets.Box([self.controls,self.map_panel],
                        layout=widgets.Layout(display='flex',flex_flow='row wrap',gap='8px',align_items='flex-start')),
            self.summary,self.result_output,self.export_output],layout=widgets.Layout(width='100%'))
        for item in (self.roi_x_start,self.roi_width,self.roi_y_start,self.roi_height,self.direction,
                     self.x0,self.y0,self.x1,self.y1):
            item.observe(self._geometry_changed,names='value')
        self.roi_x.observe(self._ranges_changed,names='value')
        self.roi_y.observe(self._ranges_changed,names='value')
        self.cross_position.observe(self._cross_changed,names='value')
        self.position.observe(self._fraction_changed,names='value')
        self.start_dist.observe(self._distance_changed,names='value')
        self.cut_length.observe(self._distance_changed,names='value')
        self.end_dist.observe(self._end_changed,names='value')
        for item in (self.spacing,self.interpolation,self.trim_invalid,self.height_view,self.annotate_extrema,
                     self.zoom_enabled,self.zoom_start,self.zoom_end):
            item.observe(self._value_changed,names='value')
        self.mouse_mode.observe(self._mode_changed,names='value')
        self.previous.on_click(lambda _:self.nudge(-1))
        self.next.on_click(lambda _:self.nudge(1))
        self.whole_line.on_click(self.select_whole_line)
        self.center_roi.on_click(self.center_region)
        self.full_roi.on_click(self.full_region)
        self.refresh_button.on_click(self.refresh)
        self.export_button.on_click(self.export)
        self.dem_export_button.on_click(self.zoom_map_to_roi)
        self.connect_button.on_click(lambda _:self.enable_mouse(force=True))
        self.pick_button.on_click(self.restore_picking)
        self._sync_geometry()
        if enable_mouse:
            self.enable_mouse()

    @staticmethod
    def _bounds(widget, low, high, value):
        if not np.isfinite([low,high,value]).all() or high < low:
            raise ValueError('控件范围无效，请重新选择区域。')
        # Validate in a legal order; send only the final range to the frontend.
        with widget.hold_sync():
            widget.max = max(widget.max,high)
            widget.min = min(widget.min,low)
            widget.value = float(np.clip(value,low,high))
            widget.min,widget.max = low,high

    @property
    def roi(self):
        return (*self.roi_x.value,*self.roi_y.value)

    def endpoints(self):
        return line_in_roi(self.roi,self.direction.value,self.position.value,
                           [(self.x0.value,self.y0.value),(self.x1.value,self.y1.value)])

    def _length(self,p0,p1):
        if self.distance_mode == 'grid':
            if self.dem.xy_factor is None:
                raise ValueError('当前坐标没有确认的米制单位，不能按米定位。')
            return float(np.linalg.norm(np.asarray(p1)-p0)*self.dem.xy_factor)
        _,metadata = extract_profile(self.dem,p0,p1,spacing_m=1e12,distance_mode=self.distance_mode)
        return metadata['full_line_length_m']

    def _sync_geometry(self):
        self._busy = True
        try:
            self._bounds(self.roi_x_start,0,max(0,self.total_x-self.roi_width.value),self.roi_x_start.value)
            self._bounds(self.roi_y_start,0,max(0,self.total_y-self.roi_height.value),self.roi_y_start.value)
            self.roi_x.value=(self.left+self.roi_x_start.value/self.factor,
                              self.left+(self.roi_x_start.value+self.roi_width.value)/self.factor)
            self.roi_y.value=(self.bottom+self.roi_y_start.value/self.factor,
                              self.bottom+(self.roi_y_start.value+self.roi_height.value)/self.factor)
            transverse = self.roi_height.value if self.direction.value=='horizontal' else self.roi_width.value
            self.cross_position.description=f'剖面 {"Y" if self.direction.value=="horizontal" else "X"} 位置 ({self.unit})'
            self._bounds(self.cross_position,0,transverse,self.position.value*transverse)
            custom = self.direction.value=='custom'
            self.cross_position.disabled=custom
            self.custom_box.layout.display='' if custom else 'none'
            self.full_length = self._length(*self.endpoints())
            if self.full_length <= self.epsilon:
                raise ValueError('剖面过短或端点重合。')
            self._sync_distance()
            self._summary()
        finally:
            self._busy = False

    def _sync_distance(self):
        # Subtracting nearly equal floats can leave slightly less than epsilon.
        # Keep bounds inside the real remainder, never max < min or beyond end.
        last_start = max(0,float(np.nextafter(self.full_length-self.epsilon,0)))
        self._bounds(self.start_dist,0,last_start,self.start_dist.value)
        remaining = self.full_length-self.start_dist.value
        self._bounds(self.cut_length,min(self.epsilon,remaining),remaining,self.cut_length.value)
        self.end_dist.value=min(self.full_length,self.start_dist.value+self.cut_length.value)
        length = self.cut_length.value
        a,b = np.clip([self.zoom_start.value,self.zoom_end.value],0,length)
        if a >= b:
            a,b = 0,length
        self._bounds(self.zoom_start,0,length,a)
        self._bounds(self.zoom_end,0,length,b)

    def _summary(self):
        self.summary.value=(f'整条线 <b>{self.full_length:.2f} m</b>；截取 '
                            f'<b>{self.start_dist.value:.2f}–{self.end_dist.value:.2f} m</b>；'
                            f'长度 <b>{self.cut_length.value:.2f} m</b><br>'
                            f'DEM X：{self.roi_x.value[0]:.3f}–{self.roi_x.value[1]:.3f}；'
                            f'DEM Y：{self.roi_y.value[0]:.3f}–{self.roi_y.value[1]:.3f}')

    def _geometry_changed(self,change=None):
        if self._busy or self._closed:
            return
        try:
            self._sync_geometry()
            self._changed()
        except Exception as exc:
            self.summary.value='<b>参数错误：</b>'+html.escape(str(exc))
            self.state.clear()

    def _ranges_changed(self,change):
        if self._busy:
            return
        self._busy=True
        try:
            xa,xb=self.roi_x.value;ya,yb=self.roi_y.value
            self.roi_width.value=max(self.epsilon,(xb-xa)*self.factor)
            self.roi_height.value=max(self.epsilon,(yb-ya)*self.factor)
            self.roi_x_start.max=self.total_x
            self.roi_y_start.max=self.total_y
            self.roi_x_start.value=float(np.clip((xa-self.left)*self.factor,0,self.roi_x_start.max))
            self.roi_y_start.value=float(np.clip((ya-self.bottom)*self.factor,0,self.roi_y_start.max))
        finally:
            self._busy=False
        self._geometry_changed()

    def _cross_changed(self,change):
        if self._busy:
            return
        self._busy=True
        self.position.value=self.cross_position.value/self.cross_position.max if self.cross_position.max else 0
        self._busy=False
        self._geometry_changed()

    def _fraction_changed(self,change):
        if not self._busy:
            self._geometry_changed()

    def _distance_changed(self,change=None):
        if self._busy:
            return
        self._busy=True
        try:
            self._sync_distance()
            self._summary()
        finally:
            self._busy=False
        self._changed()

    def _end_changed(self,change):
        if self._busy:
            return
        stop = self.full_length if self.end_dist.value==0 else self.end_dist.value
        self._busy=True
        self.cut_length.value=float(np.clip(stop-self.start_dist.value,self.cut_length.min,self.cut_length.max))
        self._busy=False
        self._distance_changed()

    def _value_changed(self,change=None):
        if not self._busy:
            self._changed()

    def _changed(self):
        self.state.pop('export_folder',None)
        self._update_overlay()
        if self.live_preview.value:
            self.refresh()

    def current_profile(self):
        table,metadata=extract_profile(self.dem,*self.endpoints(),spacing_m=self.spacing.value,
                                       start_m=self.start_dist.value,end_m=self.end_dist.value,
                                       method=self.interpolation.value,distance_mode=self.distance_mode)
        metadata.update(selector_version=5,selector_direction=self.direction.value,
                        roi_start_offset=[self.roi_x_start.value,self.roi_y_start.value])
        if self.trim_invalid.value:
            table,metadata=longest_valid_segment(table,metadata)
        return table,metadata,self.roi

    def _zoom_range(self, table):
        if not self.zoom_enabled.value:
            return None
        start,end = self.zoom_start.value,self.zoom_end.value
        if not 0 <= start < end <= float(table.x_m.iloc[-1]):
            raise ValueError(f'放大范围须满足 0 ≤ 起点 < 终点 ≤ {table.x_m.iloc[-1]:g} m；裁剪有效段后请重新确认范围。')
        return (start,end)

    def refresh(self,_=None):
        with self.result_output:
            clear_output(wait=True)
            try:
                table,metadata,roi=self.current_profile()
                zoom_range=self._zoom_range(table)
                self.state.update(table=table,metadata=metadata,roi=roi)
                print(f'{len(table)} 个样点；输出 {table.x_m.iloc[-1]:.3f} m；无效点 {metadata["invalid_samples"]}')
                if metadata.get('longest_valid_segment_applied'):
                    print('已裁剪有效段：',metadata['kept_source_distance_range_m'],'m；丢弃点数',metadata['discarded_samples'])
                fig,ax=plt.subplots(figsize=(14,4.75),layout='constrained')
                plot_profile(table,ax,height_view=self.height_view.value,
                             annotate=self.annotate_extrema.value,spacing_m=self.spacing.value)
                plt.show();plt.close(fig)
                if zoom_range is not None:
                    fig,ax=plt.subplots(figsize=(14,4.75),layout='constrained')
                    plot_profile(table,ax,height_view=self.height_view.value,
                                 annotate=self.annotate_extrema.value,spacing_m=self.spacing.value,
                                 distance_range_m=zoom_range)
                    plt.show();plt.close(fig)
                details=widgets.Accordion(children=[widgets.Output()],selected_index=None)
                details.set_title(0,'样点数据（前 5 行）')
                with details.children[0]:
                    display(table.head())
                display(details)
                if self.canvas is None:
                    # Static fallback remains available when ipympl is missing.
                    with self.map_output:
                        clear_output(wait=True)
                        fig,ax=plt.subplots(figsize=(7,3.8),layout='constrained')
                        ax.set_title('Static preview: mouse picking unavailable',fontsize=10)
                        image=plot_map(self.dem,ax,table,roi)
                        ax.set_xlim(self.left,self.right);ax.set_ylim(self.bottom,self.top)
                        fig.colorbar(image,ax=ax,label='Elevation (m)')
                        plt.show();plt.close(fig)
            except Exception as exc:
                self.state.clear()
                print(type(exc).__name__+': '+str(exc))

    def export(self,_=None):
        if self._closed or self._exporting:
            return None
        self._exporting=True
        self.export_button.disabled=True
        self.state.pop('export_folder',None)
        with self.export_output:
            clear_output(wait=True)
            try:
                if self._pending is not None:
                    raise ValueError('两点剖面尚未选完，请先点击终点，或切换点选模式取消后再导出。')
                table,metadata,roi=self.current_profile()
                self._update_overlay(table=table)
                view_bounds=None
                if self.axis is not None:
                    view_bounds=(*sorted(self.axis.get_xlim()),*sorted(self.axis.get_ylim()))
                chosen=self.export_scope.value
                if chosen=='auto':
                    whole=(self.left,self.right,self.bottom,self.top)
                    full_view=view_bounds is None or np.allclose(view_bounds,whole,rtol=0,atol=self.epsilon/self.factor)
                    cx,cy=(roi[0]+roi[1])/2,(roi[2]+roi[3])/2
                    inside=view_bounds is not None and view_bounds[0]<=cx<=view_bounds[1] and view_bounds[2]<=cy<=view_bounds[3]
                    chosen='selection' if full_view or not inside else 'viewport'
                if view_bounds is None:
                    chosen='selection'
                target=view_bounds if chosen=='viewport' else roi
                metadata['map_export_request']=dict(requested_scope=self.export_scope.value,resolved_scope=chosen,
                    current_view_bounds=list(view_bounds) if view_bounds is not None else None,
                    local_bounds=list(target),colormap='jet')
                folder=export_profile(self.dem,table,metadata,self.output_dir,roi,
                                      height_view=self.height_view.value,annotate=self.annotate_extrema.value,
                                      zoom_range_m=self._zoom_range(table),map_figure=self.figure,
                                      map_view_bounds=target,viewport_figure=self.figure,map_scope=chosen)
                self.state.update(table=table,metadata=metadata,roi=roi,export_folder=folder)
                print('已导出：',folder)
                print('模拟输入：',folder/'surface_profile.csv')
                print('局部 DEM：',folder/'dem_zoom.png','；范围来源：',chosen)
                if self.figure is not None:
                    print('当前视口快照：',folder/'dem_view.png')
                if self.figure is None:
                    print('交互画布未启用：局部图按选取矩形绘制。')
                print('所有图件、CSV 和 selection.json 均位于上述同一文件夹。')
                if self.zoom_enabled.value:
                    print('局部放大：',folder/'profile_zoom.png','及 profile_zoom.svg')
                return folder
            except Exception as exc:
                print(type(exc).__name__+': '+str(exc))
                return None
            finally:
                self._exporting=False
                self.export_button.disabled=False

    def export_dem_map(self,_=None):
        # Existing notebook calls now use the same complete export operation.
        return self.export(_)

    def zoom_map_to_roi(self,_=None):
        if self.axis is None and not self.enable_mouse():
            self.mouse_status.value='交互地图未启用；导出全部结果时按选区生成局部 DEM 图。'
            return
        self.axis.set_xlim(self.roi[0],self.roi[1])
        self.axis.set_ylim(self.roi[2],self.roi[3])
        self.canvas.draw_idle()
        self.mouse_status.value='已放大到选区。可继续平移 / 缩放，点击“导出全部结果”保存当前地图视口截图。'

    def _stop_navigation(self):
        if self.manager is None:
            return
        mode=str(self.manager.toolbar.mode).lower()
        if 'pan' in mode:
            self.manager.toolbar.pan()
        elif 'zoom' in mode:
            self.manager.toolbar.zoom()

    def restore_picking(self,_=None):
        if self._closed:
            return
        self._stop_navigation()
        target='endpoints' if self.direction.value=='custom' else 'locate'
        if self.mouse_mode.value!=target:
            self.mouse_mode.value=target
        self._mode_changed()
        self.mouse_status.value='点选已恢复：请在交互地图内点击。任意两点剖面请选择“两点剖面”。'

    def nudge(self,sign):
        self.start_dist.value=float(np.clip(self.start_dist.value+sign*self.move_step.value,0,self.start_dist.max))

    def select_whole_line(self,_=None):
        self._busy=True
        self.start_dist.value=0
        self.cut_length.max=self.full_length
        self.cut_length.value=self.full_length
        self._busy=False
        self._distance_changed()

    def set_roi(self,xmin,xmax,ymin,ymax,reset_start=False):
        xa,xb=sorted([max(self.left,min(self.right,xmin)),max(self.left,min(self.right,xmax))])
        ya,yb=sorted([max(self.bottom,min(self.top,ymin)),max(self.bottom,min(self.top,ymax))])
        if (xb-xa)*self.factor<self.epsilon or (yb-ya)*self.factor<self.epsilon:
            raise ValueError('所选区域过小。')
        self._busy=True
        self.roi_width.value=(xb-xa)*self.factor
        self.roi_height.value=(yb-ya)*self.factor
        self.roi_x_start.max=self.total_x
        self.roi_y_start.max=self.total_y
        self.roi_x_start.value=(xa-self.left)*self.factor
        self.roi_y_start.value=(ya-self.bottom)*self.factor
        if reset_start:
            self.start_dist.value=0
        self._busy=False
        self._geometry_changed()

    def full_region(self,_=None):
        self.set_roi(self.left,self.right,self.bottom,self.top,True)

    def center_region(self,_=None):
        cx,cy=(self.left+self.right)/2,(self.bottom+self.top)/2
        hx=min(500/self.factor,(self.right-self.left)*.4)
        hy=min(500/self.factor,(self.top-self.bottom)*.4)
        self.set_roi(cx-hx,cx+hx,cy-hy,cy+hy,True)

    def enable_mouse(self,force=False):
        if self._closed:
            return False
        if self.canvas is not None and not force:
            return True
        previous_view=(*self.axis.get_xlim(),*self.axis.get_ylim()) if self.axis is not None else None
        self._close_canvas()
        try:
            from ipympl.backend_nbagg import Canvas,FigureManager
            from matplotlib.figure import Figure
            from matplotlib.widgets import RectangleSelector
            # Explicit widget canvas: does not depend on the cached '%matplotlib widget' alias.
            self.figure=Figure(figsize=(7,4.1),layout='constrained')
            self.canvas=Canvas(self.figure)
            self.manager=FigureManager(self.canvas,id(self))
            self.axis=self.figure.subplots()
            image=plot_map(self.dem,self.axis,max_size=2000)
            # RectangleSelector initially creates handles near (0,0); keep them
            # from autoscaling a map whose source coordinates are ~19 million.
            self.axis.set_xlim(previous_view[:2] if previous_view is not None else (self.left,self.right))
            self.axis.set_ylim(previous_view[2:] if previous_view is not None else (self.bottom,self.top))
            self.figure.colorbar(image,ax=self.axis,label='Elevation (m)')
            self.axis.set_title('(b) DEM view',fontsize=9)
            self._roi_patch=None
            self._line_artist,=self.axis.plot([],[],color='red',lw=.9)
            self._start_artist,=self.axis.plot([],[],'ko',markersize=5)
            self._pending_artist,=self.axis.plot([],[],'mo',markersize=6)
            self._pending_artist.set_gid('pending-endpoint')
            self.rectangle_selector=RectangleSelector(self.axis,self._on_rectangle,useblit=False,button=[1],
                                                       interactive=True,minspanx=5,minspany=5,spancoords='pixels')
            for artist in self.rectangle_selector.artists:
                artist.set_gid('selection-handle')
            self._connections=[self.canvas.mpl_connect('button_press_event',self._on_press),
                               self.canvas.mpl_connect('button_release_event',self._on_release)]
            self.canvas.header_visible=False
            self.canvas.toolbar_visible=True
            self.canvas.toolbar_position='top'
            self.canvas.layout.width='100%'
            self.canvas.layout.height='390px'
            self.canvas.layout.min_height='300px'
            self._mode_changed()
            self._update_overlay()
            with self.map_output:
                clear_output(wait=True)
                display(self.canvas)
            self.mouse_status.value='<span style="color:#287844">交互画布已就绪。单击定位或切换上方点选模式；若前端未显示地图，滑条仍可使用。</span>'
            return True
        except Exception as exc:
            self._close_canvas()
            self.mouse_status.value=('交互画布未启用：'+html.escape(str(exc))+'<br>当前内核：'+html.escape(sys.executable)+
                                     '<br>在当前内核运行 <code>%pip install ipympl ipywidgets</code>，重启内核后全部运行，再点“启用 / 重建交互地图”。滑条可继续使用。')
            return False

    def _mode_changed(self,change=None):
        if self._closed:
            return
        if self.mouse_mode.value!='off':
            self._stop_navigation()
        self._pending=None
        if self.rectangle_selector is not None:
            active=self.mouse_mode.value=='rectangle'
            self.rectangle_selector.set_active(active)
            self.rectangle_selector.set_visible(active)
            self._pending_artist.set_data([],[])
            self.canvas.draw_idle()

    def _update_overlay(self,table=None):
        if self.canvas is None:
            return
        try:
            from matplotlib.patches import Rectangle
            if self._roi_patch is not None:
                self._roi_patch.remove()
            xa,xb,ya,yb=self.roi
            self._roi_patch=Rectangle((xa,ya),xb-xa,yb-ya,fill=False,edgecolor='#e47b28',lw=.7)
            self._roi_patch.set_gid('profile-roi')
            self.axis.add_patch(self._roi_patch)
            p0,p1=map(np.asarray,self.endpoints())
            if table is not None:
                selected=table[['dem_x','dem_y']].iloc[[0,-1]].to_numpy()
            elif self.distance_mode=='grid':
                selected=np.array([p0+(p1-p0)*self.start_dist.value/self.full_length,
                                   p0+(p1-p0)*self.end_dist.value/self.full_length])
            else:
                table,_,_=self.current_profile()
                selected=table[['dem_x','dem_y']].iloc[[0,-1]].to_numpy()
            self._line_artist.set_data(selected[:,0],selected[:,1])
            self._start_artist.set_data([selected[0,0]],[selected[0,1]])
            self.canvas.draw_idle()
        except Exception:
            self._line_artist.set_data([],[])
            self._start_artist.set_data([],[])

    def _toolbar_busy(self):
        return self.manager is not None and bool(self.manager.toolbar.mode)

    def _on_press(self,event):
        self._press=None
        if self._closed or event.button!=1 or event.inaxes is not self.axis:
            return
        if self._toolbar_busy():
            self.mouse_status.value='地图正处于平移 / 缩放模式，当前操作不会定位；点击“恢复点选”后再单击。'
            return
        if self.mouse_mode.value=='off':
            self.mouse_status.value='点选已关闭，点击“恢复点选”后可继续定位。'
            return
        self._press=(event.x,event.y)

    def _on_release(self,event):
        press,self._press=self._press,None
        if (self._closed or press is None or event.button!=1 or event.inaxes is not self.axis or self._toolbar_busy() or
            self.mouse_mode.value not in ('locate','endpoints') or
            math.hypot(event.x-press[0],event.y-press[1])>5*max(1,self.canvas.device_pixel_ratio)):
            return
        try:
            self._apply_click(event.xdata,event.ydata)
        except Exception as exc:
            self.mouse_status.value='点选失败：'+html.escape(str(exc))

    def _on_rectangle(self,start,end):
        if self.mouse_mode.value!='rectangle' or self._toolbar_busy():
            return
        try:
            self.set_roi(start.xdata,end.xdata,start.ydata,end.ydata,True)
        except Exception as exc:
            self.mouse_status.value=html.escape(str(exc))

    def _apply_click(self,x,y):
        x=float(np.clip(x,self.left,np.nextafter(self.right,self.left)))
        y=float(np.clip(y,self.bottom,np.nextafter(self.top,self.bottom)))
        if self.mouse_mode.value=='locate':
            if self.direction.value=='custom':
                self.mouse_status.value='任意两点模式请使用“点选两个端点”。'
                return
            w,h=self.roi_width.value/self.factor,self.roi_height.value/self.factor
            xa=x if self.direction.value=='horizontal' else x-w/2
            ya=y-h/2 if self.direction.value=='horizontal' else y
            xa=float(np.clip(xa,self.left,self.right-w));ya=float(np.clip(ya,self.bottom,self.top-h))
            self._busy=True
            self.position.value=float(np.clip((y-ya)/h if self.direction.value=='horizontal' else (x-xa)/w,0,1))
            self._busy=False
            self.set_roi(xa,xa+w,ya,ya+h,True)
            self.mouse_status.value=f'已定位剖面：X={self.endpoints()[0][0]:.3f}，Y={self.endpoints()[0][1]:.3f}。'
        elif self.mouse_mode.value=='endpoints':
            if self._pending is None:
                self._pending=(x,y)
                self._pending_artist.set_data([x],[y])
                self.mouse_status.value='已选起点，请单击终点。'
                self.canvas.draw_idle()
                return
            p0,p1=self._pending,(x,y)
            self._pending=None
            if np.linalg.norm(np.subtract(p1,p0))*self.factor<self.epsilon:
                self.mouse_status.value='两端点距离过小，请重新选择。'
                return
            pad_x=max(abs(p1[0]-p0[0])*.03,self.dem.source.res[0])
            pad_y=max(abs(p1[1]-p0[1])*.03,self.dem.source.res[1])
            self._busy=True
            self.x0.value,self.y0.value=p0
            self.x1.value,self.y1.value=p1
            self.direction.value='custom'
            self._busy=False
            self.set_roi(min(p0[0],p1[0])-pad_x,max(p0[0],p1[0])+pad_x,
                          min(p0[1],p1[1])-pad_y,max(p0[1],p1[1])+pad_y,True)
            self._pending_artist.set_data([],[])
            self.select_whole_line()
            self.mouse_status.value='已选两个端点，可用截取起点和长度滑条进一步调整。'

    def _close_canvas(self):
        if self.canvas is not None:
            for connection in self._connections:
                self.canvas.mpl_disconnect(connection)
            if self.rectangle_selector is not None:
                self.rectangle_selector.disconnect_events()
            self.canvas.close()
        self.canvas=self.manager=self.figure=self.axis=self.rectangle_selector=None
        self._press=self._pending=None
        self._connections=[]

    def close(self):
        self._closed=True
        self._close_canvas()
