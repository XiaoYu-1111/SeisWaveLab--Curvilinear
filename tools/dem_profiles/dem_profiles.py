"""Read a DEM and extract documented, metre-based surface profiles.

The input is always read-only. Generated raster caches and exports belong in out/.
The fixed-record fallback intentionally supports only unrotated metric USGS-style
profiles; it does not invent a geographic datum or an EPSG identifier.
"""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import pandas as pd
import rasterio
from rasterio import Affine
from rasterio.enums import Resampling
from rasterio.errors import RasterioIOError
from rasterio.windows import Window


def project_output() -> Path:
    for parent in (Path.cwd(), *Path.cwd().parents):
        if (parent / 'CMakePresets.json').is_file() and (parent / 'src').is_dir():
            return parent.parent / 'out/dem_profiles'
        if (parent / 'SeisWaveLab_Curvilinear/CMakePresets.json').is_file():
            return parent / 'out/dem_profiles'
    return Path.cwd() / 'out/dem_profiles'


def _number(record: bytes, start: int, width: int, integer=False):
    text = record[start:start + width].decode('ascii').strip().replace('D', 'E')
    if not text:
        raise ValueError(f'Empty fixed-width DEM field at byte {start}.')
    value = int(text) if integer else float(text)
    if not math.isfinite(value):
        raise ValueError('Non-finite DEM header value.')
    return value


def read_metric_usgs_records(path: Path):
    """Validated 1024-byte A/B record reader for the supplied metric DEM variant.

    Raster values are height * z_resolution + local_profile_datum. B profile
    samples run south to north; output raster rows run north to south.
    """
    with path.open('rb') as stream:
        header = stream.read(1024)
        if len(header) != 1024:
            raise ValueError('Not a 1024-byte USGS-style DEM header.')
        pattern = _number(header, 150, 6, True)
        system = _number(header, 156, 6, True)
        h_unit = _number(header, 528, 6, True)
        v_unit = _number(header, 534, 6, True)
        if pattern != 1 or system != 9 or h_unit != 2 or v_unit not in (1, 2):
            raise ValueError('Fallback requires regular projection-code-9 profiles with horizontal metres.')
        if abs(_number(header, 786, 24)) > 1e-12:
            raise ValueError('Rotated fixed-record DEM requires conversion by its exporting GIS.')
        corners = np.array([_number(header, 546 + i * 24, 24) for i in range(8)]).reshape(4, 2)
        xmin, ymin = corners.min(axis=0)
        xmax, ymax = corners.max(axis=0)
        dx, dy, dz = (_number(header, 816 + i * 12, 12) for i in range(3))
        width = _number(header, 858, 6, True)
        n_profile_rows = _number(header, 852, 6, True)
        if n_profile_rows != 1 or min(dx, dy, dz) <= 0 or not 1 <= width <= 100_000:
            raise ValueError('Invalid DEM dimensions or sample spacing.')
        height = int(round((ymax - ymin) / dy)) + 1
        if height < 1 or width * height > 25_000_000:
            raise ValueError('Fixed-record DEM exceeds this notebook reader\'s 25-million-pixel limit.')
        tolerance = max(dx, dy) * 2e-5
        if abs(xmax - xmin - (width - 1) * dx) > tolerance:
            raise ValueError('Header X extent is inconsistent with profile count and spacing.')
        if abs(ymax - ymin - (height - 1) * dy) > tolerance:
            raise ValueError('Header Y extent is inconsistent with sample spacing.')
        values = np.full((height, width), np.nan, dtype=np.float64)
        for column in range(width):
            record = stream.read(1024)
            if len(record) != 1024:
                raise ValueError(f'Truncated B record at column {column + 1}.')
            row_id, col_id, count, ncols = (_number(record, 6 * i, 6, True) for i in range(4))
            if row_id != 1 or col_id != column + 1 or ncols != 1 or not 1 <= count <= height:
                raise ValueError(f'Unexpected profile numbering/dimensions at column {column + 1}.')
            x0, y0, local_datum, zlo, zhi = (_number(record, 24 + 24 * i, 24) for i in range(5))
            if abs(x0 - (xmin + column * dx)) > tolerance:
                raise ValueError(f'Profile X alignment failed at column {column + 1}.')
            chunks = [record[144:144 + min(count, 146) * 6]]
            remaining = max(0, count - 146)
            while remaining:
                continuation = stream.read(1024)
                if len(continuation) != 1024:
                    raise ValueError('Truncated DEM continuation record.')
                take = min(remaining, 170)
                chunks.append(continuation[:take * 6])
                remaining -= take
            try:
                raw = np.frombuffer(b''.join(chunks), dtype='S6').astype(np.int32)
            except ValueError as exc:
                raise ValueError('Invalid fixed-width elevation sample.') from exc
            valid = raw != -32767
            elevations = raw.astype(np.float64) * dz + local_datum
            northings = y0 + np.arange(count) * dy
            positions = (ymax - northings) / dy
            indices = np.rint(positions).astype(np.int64)
            if np.any(np.abs(positions - indices) * dy > tolerance) or np.any((indices < 0) | (indices >= height)):
                raise ValueError('Profile Y alignment or coverage failed.')
            if valid.any() and (elevations[valid].min() < zlo - 2 * dz or elevations[valid].max() > zhi + 2 * dz):
                raise ValueError('Decoded profile heights exceed the B-record declared range.')
            values[indices[valid], column] = elevations[valid]
        global_min, global_max = _number(header, 738, 24), _number(header, 762, 24)
        finite = values[np.isfinite(values)]
        if not finite.size or finite.min() < global_min - 2 * dz or finite.max() > global_max + 2 * dz:
            raise ValueError('Decoded heights do not match the A-record range.')
        metadata = dict(reader='metric_USGS_fixed_records', projection_code=system,
                        projection_parameters=[_number(header, 168 + 24 * i, 24) for i in range(15)],
                        horizontal_unit='m', vertical_unit='m' if v_unit == 2 else 'ft',
                        original_z_resolution=dz, original_height_range=[global_min, global_max],
                        coordinate_datum='not_assigned', header_sha256=hashlib.sha256(header).hexdigest(),
                        pixel_center_extent=[xmin, ymin, xmax, ymax])
        transform = Affine(dx, 0, xmin - dx / 2, 0, -dy, ymax + dy / 2)
    return values, transform, metadata


def unit_to_metres(unit):
    name = str(unit or '').strip().lower().replace(' ', '_')
    if name in ('m', 'metre', 'meter', 'metres', 'meters'):
        return 1.0
    if name in ('ft', 'foot', 'feet', 'international_foot'):
        return .3048
    if name in ('us_survey_foot', 'us_survey_feet', 'ftus', 'ft_us'):
        return 1200 / 3937
    raise ValueError(f'Unknown unit {unit!r}; set an explicit unit override after checking the source.')


class DEM:
    def __init__(self, path, output_dir=None, crs_override=None, xy_unit_override=None, z_unit_override=None):
        self.path = Path(path).expanduser().resolve()
        self.output_dir = Path(output_dir or project_output()).resolve()
        self.extra = {}
        self.fallback = False
        self.source = None
        if not self.path.is_file():
            raise FileNotFoundError(self.path)
        try:
            self.source = rasterio.open(self.path, 'r')
        except RasterioIOError as original_error:
            try:
                data, transform, self.extra = read_metric_usgs_records(self.path)
            except Exception as exc:
                raise ValueError(f'GDAL could not open the raster; fixed-record fallback also failed: {exc}') from original_error
            self.fallback = True
            cache = self.output_dir / 'cache'
            cache.mkdir(parents=True, exist_ok=True)
            # Timestamp/size distinguish changed inputs, not only their shared A header.
            identity = f'fixed-record-v2-float64|{self.path}|{self.path.stat().st_size}|{self.path.stat().st_mtime_ns}'
            key = hashlib.sha256(identity.encode()).hexdigest()[:12]
            cache_path = cache / f'{self.path.stem}-{key}.tif'
            if not cache_path.exists():
                with rasterio.open(cache_path, 'w', driver='GTiff', height=data.shape[0], width=data.shape[1],
                                   count=1, dtype='float64', transform=transform, nodata=np.nan,
                                   compress='deflate') as dst:
                    dst.write(data, 1)
                    dst.set_band_unit(1, self.extra['vertical_unit'])
                    dst.update_tags(SOURCE_FORMAT='USGS-style projection code 9; geographic datum unresolved')
            self.source = rasterio.open(cache_path, 'r')
            if self.source.shape != data.shape or self.source.transform != transform or self.source.dtypes[0] != 'float64':
                self.source.close()
                raise ValueError('Existing DEM cache is inconsistent; use a new output directory.')
        try:
            from pyproj import CRS
            self.crs = CRS.from_user_input(crs_override or self.source.crs) if (crs_override or self.source.crs) else None
            if self.crs is not None and self.crs.is_compound:
                self.crs = next(c for c in self.crs.sub_crs_list if c.is_projected or c.is_geographic)
            if xy_unit_override:
                self.xy_factor = unit_to_metres(xy_unit_override)
            elif self.extra.get('horizontal_unit') == 'm':
                self.xy_factor = 1.0
            elif self.crs is not None and self.crs.is_projected:
                factors = [a.unit_conversion_factor for a in self.crs.axis_info[:2]]
                if len(factors) != 2 or not math.isclose(*factors, rel_tol=1e-10):
                    raise ValueError('Projected axes need compatible length units.')
                self.xy_factor = factors[0]
            else:
                self.xy_factor = None
            self.z_unit = z_unit_override or self.source.units[0]
            self.z_factor = unit_to_metres(self.z_unit)
            self.scale = self.source.scales[0]
            self.offset = self.source.offsets[0]
            if not np.isfinite([self.scale, self.offset]).all() or self.scale == 0:
                raise ValueError('Invalid raster height scale/offset.')
        except Exception:
            self.close()
            raise

    def close(self):
        if self.source is not None:
            self.source.close()

    @property
    def bounds(self):
        return self.source.bounds

    def info(self):
        s = self.source
        return dict(input=str(self.path), driver=s.driver, fallback_reader=self.fallback,
                    shape=[s.height, s.width], resolution=list(s.res), bounds=list(s.bounds),
                    transform=list(s.transform), crs=self.crs.to_string() if self.crs else None,
                    horizontal_metres_per_unit=self.xy_factor, height_unit=self.z_unit,
                    height_to_metres=self.z_factor, raster_scale=self.scale, raster_offset=self.offset,
                    nodata=s.nodata if s.nodata is None or np.isfinite(s.nodata) else 'NaN',
                    vertical_datum_conversion='none', original_header=self.extra)

    def preview(self, max_size=850, bounds=None):
        s = self.source
        if max_size < 1:
            raise ValueError('Preview size must be positive.')
        window = Window(0,0,s.width,s.height)
        if bounds is not None:
            xmin,xmax,ymin,ymax = map(float,bounds)
            if not np.isfinite([xmin,xmax,ymin,ymax]).all() or xmin >= xmax or ymin >= ymax:
                raise ValueError('DEM zoom bounds must be finite with positive width and height.')
            corners = np.array([(~s.transform)*(x,y) for x in (xmin,xmax) for y in (ymin,ymax)])
            c0,r0 = np.maximum(0,np.floor(corners.min(axis=0))).astype(int)
            c1,r1 = np.minimum([s.width,s.height],np.ceil(corners.max(axis=0))).astype(int)
            if c1 <= c0 or r1 <= r0:
                raise ValueError('DEM zoom region does not overlap the raster.')
            window = Window(int(c0),int(r0),int(c1-c0),int(r1-r0))
        ratio = min(1.0, max_size / max(window.width,window.height))
        h, w = max(1,int(window.height*ratio)),max(1,int(window.width*ratio))
        z = s.read(1,window=window,out_shape=(h,w),masked=True,
                   resampling=Resampling.nearest).astype('float64').filled(np.nan)
        z = (z * self.scale + self.offset) * self.z_factor
        t = s.window_transform(window) * Affine.scale(window.width/w,window.height/h)
        col, row = np.meshgrid(np.arange(w + 1), np.arange(h + 1))
        x = t.a * col + t.b * row + t.c
        y = t.d * col + t.e * row + t.f
        return x, y, z

    def sample(self, x, y, method='bilinear'):
        x, y = np.asarray(x, dtype=float), np.asarray(y, dtype=float)
        if x.ndim != 1 or x.shape != y.shape or not np.isfinite([x, y]).all():
            raise ValueError('Sampling coordinates must be finite vectors of equal length.')
        s, inv = self.source, ~self.source.transform
        c, r = inv.a*x + inv.b*y + inv.c, inv.d*x + inv.e*y + inv.f
        inside = (c >= 0) & (r >= 0) & (c < s.width) & (r < s.height)
        result = np.full(x.size, np.nan)
        if not inside.any():
            return result
        if method == 'nearest':
            cc, rr = np.floor(c[inside]).astype(int), np.floor(r[inside]).astype(int)
            columns, rows = [cc], [rr]
            weights = np.ones((1, cc.size))
        elif method == 'bilinear':
            u, v = np.clip(c[inside]-.5, 0, s.width-1), np.clip(r[inside]-.5, 0, s.height-1)
            c0, r0 = np.floor(u).astype(int), np.floor(v).astype(int)
            c1, r1 = np.minimum(c0+1, s.width-1), np.minimum(r0+1, s.height-1)
            a, b = u-c0, v-r0
            columns, rows = [c0, c1, c0, c1], [r0, r0, r1, r1]
            weights = np.array([(1-a)*(1-b), a*(1-b), (1-a)*b, a*b])
        else:
            raise ValueError('Choose nearest or bilinear interpolation.')
        cmin, cmax = min(k.min() for k in columns), max(k.max() for k in columns)
        rmin, rmax = min(k.min() for k in rows), max(k.max() for k in rows)
        if (cmax-cmin+1)*(rmax-rmin+1) > 25_000_000:
            raise ValueError('Selected sampling window exceeds 25 million pixels; shorten the profile.')
        data = s.read(1, window=Window(int(cmin), int(rmin), int(cmax-cmin+1), int(rmax-rmin+1)),
                      masked=True).astype('float64').filled(np.nan)
        samples = np.array([data[rr-rmin, cc-cmin] for cc, rr in zip(columns, rows)])
        bad = ((~np.isfinite(samples)) & (weights > 1e-12)).any(axis=0)
        z = np.sum(np.where(np.isfinite(samples), samples, 0)*weights, axis=0)
        z = (z*self.scale + self.offset)*self.z_factor
        z[bad] = np.nan
        result[inside] = z
        return result


def line_in_roi(roi, direction='horizontal', position=.5, endpoints=None):
    xmin, xmax, ymin, ymax = map(float, roi)
    if not np.isfinite(roi).all() or not xmin < xmax or not ymin < ymax or not 0 <= position <= 1:
        raise ValueError('ROI needs xmin<xmax, ymin<ymax and position in [0,1].')
    if direction == 'horizontal':
        y = ymin + position*(ymax-ymin)
        return (xmin, y), (xmax, y)
    if direction == 'vertical':
        x = xmin + position*(xmax-xmin)
        return (x, ymin), (x, ymax)
    if direction == 'custom' and endpoints is not None:
        points = np.asarray(endpoints, dtype=float)
        if points.shape != (2, 2) or not np.isfinite(points).all():
            raise ValueError('Custom endpoints need shape (2,2).')
        if np.any((points[:,0] < xmin) | (points[:,0] > xmax) | (points[:,1] < ymin) | (points[:,1] > ymax)):
            raise ValueError('Both custom endpoints must lie inside the ROI.')
        return tuple(points[0]), tuple(points[1])
    raise ValueError('Choose horizontal, vertical or custom direction.')


def extract_profile(dem, p0, p1, spacing_m=5.0, start_m=0.0, end_m=None,
                    method='bilinear', distance_mode='grid'):
    points = np.asarray([p0, p1], dtype=float)
    if points.shape != (2,2) or not np.isfinite(points).all() or np.all(points[0] == points[1]):
        raise ValueError('Need two different finite endpoints in DEM coordinates.')
    if not np.isfinite([spacing_m, start_m]).all() or spacing_m < 1e-4 or start_m < 0:
        raise ValueError('Spacing must be at least 0.0001 m; start distance must be nonnegative.')
    t = np.linspace(0, 1, 4097)
    x, y = points[0,:,None] + (points[1]-points[0])[:,None]*t
    lon = lat = None
    if distance_mode == 'grid':
        if dem.xy_factor is None:
            raise ValueError('Grid distance needs declared projected length units; do not treat degrees as metres.')
        distance = np.r_[0, np.cumsum(np.hypot(np.diff(x), np.diff(y))*dem.xy_factor)]
    elif distance_mode == 'geodesic':
        if dem.crs is None:
            raise ValueError('Geodesic distance requires a confirmed CRS, which this DEM does not declare reliably.')
        from pyproj import Transformer
        lon, lat = Transformer.from_crs(dem.crs, dem.crs.geodetic_crs, always_xy=True).transform(x, y)
        _, _, lengths = dem.crs.get_geod().inv(lon[:-1], lat[:-1], lon[1:], lat[1:])
        distance = np.r_[0, np.cumsum(lengths)]
    else:
        raise ValueError('Choose grid or geodesic distance.')
    if not np.isfinite(distance).all() or distance[-1] <= 0:
        raise ValueError('Invalid profile length; check coordinate units and CRS.')
    stop = float(distance[-1] if end_m is None else end_m)
    if not np.isfinite(stop) or not start_m < stop or stop > distance[-1] + 1e-6:
        raise ValueError(f'Distance range must satisfy 0 <= start < end <= {distance[-1]:.6f} m.')
    stop = min(stop, float(distance[-1]))
    if (stop-start_m)/spacing_m > 1_000_000:
        raise ValueError('More than one million samples requested; increase sampling spacing.')
    sample_distances = np.arange(start_m, stop, spacing_m)
    if stop-sample_distances[-1] < 1e-7:
        sample_distances[-1] = stop
    else:
        sample_distances = np.r_[sample_distances, stop]
    sx, sy = np.interp(sample_distances, distance, x), np.interp(sample_distances, distance, y)
    z = dem.sample(sx, sy, method)
    table = pd.DataFrame(dict(x_m=sample_distances-start_m, elevation_m=z, dem_x=sx, dem_y=sy,
                              source_distance_m=sample_distances, valid=np.isfinite(z)))
    if lon is not None:
        table['longitude_deg'] = np.interp(sample_distances, distance, lon)
        table['latitude_deg'] = np.interp(sample_distances, distance, lat)
    metadata = dict(dem=dem.info(), endpoints_dem=points.tolist(), full_line_length_m=float(distance[-1]),
                    selected_distance_range_m=[float(start_m),stop], distance_mode=distance_mode,
                    distance_definition='horizontal projected grid length' if distance_mode == 'grid' else 'ellipsoidal horizontal length along the selected line',
                    requested_spacing_m=float(spacing_m), last_interval_m=float(np.diff(table.x_m)[-1]),
                    interpolation=method, height_processing='source scale/offset and unit-to-metre conversion only',
                    smoothing='none', datum_shift_m=0, invalid_samples=int((~table.valid).sum()))
    return table, metadata


def longest_valid_segment(table, metadata):
    good = table.valid.to_numpy(dtype=bool)
    starts = np.flatnonzero(np.diff(np.r_[False, good, False].astype(int)) == 1)
    ends = np.flatnonzero(np.diff(np.r_[False, good, False].astype(int)) == -1)
    if not starts.size:
        raise ValueError('No valid terrain samples along the selected line.')
    k = int(np.argmax(ends-starts))
    result = table.iloc[starts[k]:ends[k]].copy().reset_index(drop=True)
    if len(result) < 2:
        raise ValueError('Fewer than two consecutive valid samples.')
    result['x_m'] -= result.x_m.iloc[0]
    updated = dict(metadata, invalid_samples=0, longest_valid_segment_applied=True,
                   kept_source_distance_range_m=[float(result.source_distance_m.iloc[0]),float(result.source_distance_m.iloc[-1])],
                   discarded_samples=len(table)-len(result))
    return result, updated


def plot_map(dem, ax, table=None, roi=None, *, view_bounds=None, color_limits=None, max_size=850):
    from matplotlib.patches import Rectangle
    from matplotlib.ticker import MaxNLocator
    x,y,z = dem.preview(max_size=max_size,bounds=view_bounds)
    # image = ax.pcolormesh(x,y,z,shading='flat',cmap='terrain',rasterized=True)
    image = ax.pcolormesh(x,y,z,shading='flat',cmap='jet',rasterized=True)
    if color_limits is not None:
        image.set_clim(*color_limits)
    if roi is not None:
        xmin,xmax,ymin,ymax = roi
        ax.add_patch(Rectangle((xmin,ymin),xmax-xmin,ymax-ymin,fill=False,edgecolor='#db6d28',lw=1.5))
    if table is not None:
        ax.plot(table.dem_x,table.dem_y,color='red',lw=1.6,label='Selected profile')
        ax.scatter([table.dem_x.iloc[0]],[table.dem_y.iloc[0]],color='black',s=24,zorder=3)
    metric = dem.xy_factor == 1.0
    ax.set(xlabel='X (m)' if metric else 'DEM X (source units)',
           ylabel='Y (m)' if metric else 'DEM Y (source units)')
    ax.ticklabel_format(style='plain',useOffset=False)
    ax.xaxis.set_major_locator(MaxNLocator(4))
    ax.yaxis.set_major_locator(MaxNLocator(5))
    ax.set_aspect('equal',adjustable='box')
    publication_axes(ax)
    if view_bounds is not None:
        ax.set_xlim(view_bounds[0],view_bounds[1])
        ax.set_ylim(view_bounds[2],view_bounds[3])
        ax.set_aspect('equal',adjustable='box')
    return image


def publication_axes(ax):
    """Consistent print typography without changing global notebook settings."""
    ax.tick_params(which='both',direction='in',top=True,right=True,labelsize=8,width=.6)
    for spine in ax.spines.values():
        spine.set_linewidth(.6)
    for label in [ax.xaxis.label,ax.yaxis.label,ax.title,*ax.get_xticklabels(),*ax.get_yticklabels()]:
        label.set_fontfamily('Times New Roman')
        label.set_fontsize(9 if label in (ax.xaxis.label,ax.yaxis.label,ax.title) else 8)
    for label in ax.texts:
        label.set_fontfamily('Times New Roman')
        label.set_fontsize(8)


def save_publication_figure(figure, folder, stem):
    """One folder, consistent filenames: 600 dpi PNG plus vector PDF/SVG."""
    folder=Path(folder)
    folder.mkdir(parents=True,exist_ok=True)
    for axis in figure.axes:
        publication_axes(axis)
    figure.savefig(folder/f'{stem}.png',dpi=600,facecolor='white')
    figure.savefig(folder/f'{stem}.pdf',facecolor='white')
    figure.savefig(folder/f'{stem}.svg',facecolor='white')


def _dem_detail_figsize(bounds):
    xmin,xmax,ymin,ymax=map(float,bounds)
    height=float(np.clip(6.1*(ymax-ymin)/(xmax-xmin)+1.45,2.4,8.5))
    return (7.2,height)


def _dem_detail_colorbar(figure,image,ax):
    return figure.colorbar(image,ax=ax,label='Elevation (m)',orientation='horizontal',
                           location='bottom',pad=.14,fraction=.055,aspect=40,shrink=.9)


def snapshot_map_figure(source, bounds=None):
    """Freeze visible map data in an Agg figure; never save the ipympl canvas.

    Reusing the live canvas for high-DPI export can send resize/renderer updates
    to the frontend. Copy the existing mesh, scale and overlays instead.
    """
    from matplotlib.figure import Figure
    from matplotlib.backends.backend_agg import FigureCanvasAgg
    from matplotlib.collections import QuadMesh
    from matplotlib.patches import Rectangle
    if not source.axes:
        raise ValueError('Interactive map has no axes.')
    original=source.axes[0]
    mesh=next((item for item in original.collections if isinstance(item,QuadMesh)),None)
    if mesh is None:
        raise ValueError('Interactive map background is unavailable; rebuild the map.')
    view=tuple(map(float,bounds if bounds is not None else (*original.get_xlim(),*original.get_ylim())))
    if len(view)!=4 or not np.isfinite(view).all() or view[0]>=view[1] or view[2]>=view[3]:
        raise ValueError('Invalid map snapshot bounds.')
    result=Figure(figsize=_dem_detail_figsize(view),layout='constrained')
    FigureCanvasAgg(result)
    ax=result.subplots()
    coords=mesh.get_coordinates()
    values=np.ma.array(mesh.get_array(),copy=True).reshape(coords.shape[0]-1,coords.shape[1]-1)
    image=ax.pcolormesh(coords[:,:,0],coords[:,:,1],values,shading='flat',
                        cmap=mesh.get_cmap(),vmin=mesh.get_clim()[0],vmax=mesh.get_clim()[1],rasterized=True)
    for line in original.lines:
        if line.get_visible() and line.get_gid() not in ('selection-handle','pending-endpoint'):
            ax.plot(*line.get_data(),color=line.get_color(),lw=line.get_linewidth(),
                    linestyle=line.get_linestyle(),marker=line.get_marker(),
                    markersize=line.get_markersize(),zorder=line.get_zorder())
    for patch in original.patches:
        if isinstance(patch,Rectangle) and patch.get_gid()=='profile-roi' and patch.get_visible():
            ax.add_patch(Rectangle(patch.get_xy(),patch.get_width(),patch.get_height(),fill=False,
                                  edgecolor=patch.get_edgecolor(),lw=.7))
    for text in original.texts:
        if text.get_visible():
            trans=ax.transAxes if text.get_transform() is original.transAxes else ax.transData
            ax.text(*text.get_position(),text.get_text(),transform=trans,color=text.get_color(),
                    ha=text.get_ha(),va=text.get_va(),fontsize=8)
    ax.set(xlim=view[:2],ylim=view[2:],xlabel=original.get_xlabel(),ylabel=original.get_ylabel(),
           title='(b) Enlarged DEM view',aspect='equal')
    from matplotlib.ticker import MaxNLocator
    ax.ticklabel_format(style='plain',useOffset=False)
    ax.xaxis.set_major_locator(MaxNLocator(4));ax.yaxis.set_major_locator(MaxNLocator(5))
    _dem_detail_colorbar(result,image,ax)
    return result


def _write_dem_zoom(dem, bounds, folder, table=None, color_limits=None):
    """Read the selected raster window, not a magnified overview screenshot."""
    import matplotlib.pyplot as plt
    bounds = tuple(map(float,bounds))
    _,_,local = dem.preview(max_size=2000,bounds=bounds)
    if not np.isfinite(local).any():
        raise ValueError('DEM zoom region has no valid elevation pixels.')
    if color_limits is None:
        _,_,overview = dem.preview()
        valid = overview[np.isfinite(overview)]
        if not valid.size:
            raise ValueError('DEM overview has no valid elevations for a shared color scale.')
        color_limits = (float(valid.min()),float(valid.max()))
    if color_limits[0] == color_limits[1]:
        color_limits = (color_limits[0]-.5,color_limits[1]+.5)
    fig,ax=plt.subplots(figsize=_dem_detail_figsize(bounds),layout='constrained')
    try:
        image=plot_map(dem,ax,table,view_bounds=bounds,color_limits=color_limits,max_size=2000)
        _dem_detail_colorbar(fig,image,ax)
        ax.set_title('(b) Local DEM')
        folder.mkdir(parents=True,exist_ok=True)
        save_publication_figure(fig,folder,'dem_zoom')
    finally:
        plt.close(fig)
    return dict(bounds_dem_coordinates=list(bounds),color_limits_m=list(map(float,color_limits)),
                color_scale='shared with full DEM overview',raster_shape=list(local.shape),
                sampling='native selected raster window; nearest-neighbour reduction only above 2000 pixels',
                height_processing='source scale/offset and units applied; no smoothing or datum shift',
                profile_overlay=table is not None)


def export_dem_zoom(dem, bounds, output_dir=None, table=None):
    """Export only the selected DEM map, independently of profile CSV export."""
    folder=Path(output_dir or dem.output_dir)/datetime.now().strftime('dem_zoom_%Y%m%d_%H%M%S_%f')
    detail=_write_dem_zoom(dem,bounds,folder,table)
    (folder/'dem_zoom_selection.json').write_text(json.dumps(dict(
        exported_utc=datetime.now(timezone.utc).isoformat(),dem=dem.info(),dem_map_zoom=detail),
        ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
    return folder


def profile_plot_reference(table, height_view='absolute'):
    """Display reference only; never modify sampled/exported elevations."""
    if height_view not in ('absolute', 'start', 'mean'):
        raise ValueError('Height view must be absolute, start or mean.')
    heights = np.asarray(table.elevation_m, dtype=float)
    good = heights[np.isfinite(heights)]
    if not good.size:
        raise ValueError('No finite elevation samples to plot.')
    if height_view == 'mean':
        return float(good.mean())
    # An invalid first sample is not an assumed zero-height datum.
    return float(good[0])


def profile_plot_window(table, distance_range_m):
    """Clip at exact distances with linear boundary interpolation, no smoothing.

    Interpolation only joins adjacent finite samples, preserving NoData gaps.
    Returned distances remain relative to the full exported profile start.
    """
    x = np.asarray(table.x_m,dtype=float)
    bounds = np.asarray(distance_range_m,dtype=float)
    if (bounds.shape != (2,) or not np.isfinite(bounds).all() or len(x) < 2 or
        not np.isfinite(x).all() or not (np.diff(x) > 0).all() or
        not x[0] <= bounds[0] < bounds[1] <= x[-1]):
        raise ValueError('Zoom range must lie inside the profile with start < end.')
    a,b = bounds
    z = np.asarray(table.elevation_m,dtype=float)
    def at(position):
        i = int(np.searchsorted(x,position))
        if i < len(x) and x[i] == position:
            return z[i]
        if not np.isfinite(z[i-1:i+1]).all():
            return np.nan
        return float(np.interp(position,x[i-1:i+1],z[i-1:i+1]))
    inside = (x > a) & (x < b)
    return pd.DataFrame(dict(x_m=np.r_[a,x[inside],b], elevation_m=np.r_[at(a),z[inside],at(b)]))


def plot_profile(table, ax, *, height_view='absolute', annotate=True, spacing_m=None, distance_range_m=None):
    """Wide profile with a declared datum, two fills and true extrema.

    Missing values remain gaps. Width arrows are deliberately omitted: natural
    DEM terrain has no unique hill/valley width without a defined measurement.
    """
    from matplotlib.ticker import MaxNLocator, ScalarFormatter
    reference = profile_plot_reference(table, height_view)
    first_invalid = not np.isfinite(table.elevation_m.iloc[0])
    if spacing_m is None:
        steps = np.diff(np.asarray(table.x_m,dtype=float))
        steps = steps[np.isfinite(steps) & (steps > 0)]
        spacing_m = float(np.median(steps)) if steps.size else None
    if distance_range_m is not None:
        table = profile_plot_window(table,distance_range_m)
    x = np.asarray(table.x_m, dtype=float)
    original = np.asarray(table.elevation_m, dtype=float)
    y = original if height_view == 'absolute' else original-reference
    baseline = reference if height_view == 'absolute' else 0.0
    valid = np.isfinite(x) & np.isfinite(y)
    if not valid.any():
        raise ValueError('No finite distance/elevation pairs to plot.')
    domain = x[np.isfinite(x)]
    ax.set_facecolor('white')
    ax.set_axisbelow(True)
    ax.grid(True, color='#bcbcbc', alpha=.24, linewidth=.35)
    ax.axhline(baseline, color='#858b8c', lw=.45, zorder=2)
    ax.fill_between(x, y, baseline, where=valid & (y >= baseline),
                    interpolate=True, color='#a8bfa3', alpha=.28, linewidth=0, zorder=1)
    ax.fill_between(x, y, baseline, where=valid & (y <= baseline),
                    interpolate=True, color='#a1c6d5', alpha=.28, linewidth=0, zorder=1)
    ax.plot(x, y, color='#28566a', lw=.85, solid_capstyle='round',solid_joinstyle='round',zorder=3)
    length = float(domain.max()-domain.min())
    intervals = np.diff(x)
    intervals = intervals[np.isfinite(intervals) & (intervals > 0)]
    spacing = float(spacing_m) if spacing_m is not None else (float(np.median(intervals)) if intervals.size else None)
    length_label = f'{length/1000:g} km' if length >= 1000 else f'{length:g} m'
    sampling = f' | {spacing:g} m sampling' if spacing is not None else ''
    reference_label = ('first valid sample' if first_invalid else 'start') if height_view != 'mean' else 'sample mean'
    if distance_range_m is not None:
        reference_label = 'full profile '+reference_label
    datum = f'{reference_label} = {reference:.2f} m'
    title = f'{length_label} elevation profile{sampling}'
    if distance_range_m is not None:
        title = f'{domain.min():g}–{domain.max():g} m detail ({length_label}){sampling}'
    ax.set_title(title+'\n'+('Fill reference: ' if height_view == 'absolute' else 'Relative to ')+datum,
                 fontsize=14, pad=10)
    ylabel = {'absolute':'Elevation (m)', 'start':'Elevation relative to start (m)',
              'mean':'Elevation relative to sample mean (m)'}[height_view]
    if height_view == 'start' and first_invalid:
        ylabel = 'Elevation relative to first valid sample (m)'
    ax.set(xlabel='Horizontal distance (m)', ylabel=ylabel)
    ax.xaxis.label.set_size(12)
    ax.yaxis.label.set_size(12)
    ax.tick_params(direction='out', labelsize=11, length=4, width=.8)
    ax.xaxis.set_major_locator(MaxNLocator(nbins=10))
    ax.yaxis.set_major_locator(MaxNLocator(nbins=6))
    for axis in (ax.xaxis, ax.yaxis):
        formatter = ScalarFormatter(useOffset=False)
        formatter.set_scientific(False)
        axis.set_major_formatter(formatter)
    lo, hi = float(y[valid].min()), float(y[valid].max())
    span = max(hi-lo, 1.0)
    ax.set_ylim(lo-.24*span, hi+.24*span)
    if length > 0:
        ax.set_xlim(float(domain.min()), float(domain.max()))
    if annotate:
        candidates = [int(np.flatnonzero(valid)[np.argmin(y[valid])]),
                      int(np.flatnonzero(valid)[np.argmax(y[valid])])]
        for index in dict.fromkeys(candidates):
            is_high = y[index] == hi
            fraction = (x[index]-domain.min())/length if length else .5
            align = 'left' if fraction < .12 else ('right' if fraction > .88 else 'center')
            ax.annotate(f'({x[index]:g} m, {y[index]:+.2f} m)' if height_view != 'absolute'
                        else f'({x[index]:g} m, {y[index]:.2f} m)',
                        (x[index], y[index]), xytext=(0, 12 if is_high else -14),
                        textcoords='offset points', ha=align, va='bottom' if is_high else 'top',
                        fontsize=11, annotation_clip=False)
    for spine in ax.spines.values():
        spine.set_linewidth(.9)
    return reference


def surface_grid_geometry(dem, roi, nx=300, ny=None, *, spacing_m=None, endpoint=False):
    """Plan square spacing without allocating an array.

    With ny=None the exact ROI is retained: X count determines h and Y count is
    derived. Default sampling is [start, end), so 1000 m / 500 points = 2 m.
    Explicit nx,ny retains the legacy centred-inscribed shape option.
    """
    if dem.xy_factor is None:
        raise ValueError('区域导出需要已确认的水平长度单位，不能将经纬度直接作为米。')
    bounds=np.asarray(roi,dtype=float)
    if bounds.shape!=(4,) or not np.isfinite(bounds).all():
        raise ValueError('区域范围应为有限的 xmin,xmax,ymin,ymax。')
    xmin,xmax,ymin,ymax=map(float,bounds)
    if xmin>=xmax or ymin>=ymax:
        raise ValueError('区域宽度与高度必须为正。')
    width_requested,height_requested=(xmax-xmin)*dem.xy_factor,(ymax-ymin)*dem.xy_factor
    def count(value):
        if isinstance(value,(bool,np.bool_)) or not np.isfinite(value) or int(value)!=value or not 2<=value<=4096:
            raise ValueError('区域数组每轴须为 2..4096 个整数点。')
        return int(value)
    def axis_count(length,h):
        ratio=length/h
        # Absorb numerical noise only near integer boundaries.
        if abs(ratio-round(ratio))<1e-9:ratio=float(round(ratio))
        return int(np.floor(ratio))+1 if endpoint else int(np.ceil(ratio))
    inset=spacing_m is None and ny is not None
    if spacing_m is not None:
        h=float(spacing_m)
    else:
        nx=count(nx)
        h=width_requested/(nx-int(endpoint))
        if ny is not None:
            ny=count(ny)
            h=min(h,height_requested/(ny-int(endpoint)))
    if not np.isfinite(h) or h<1e-4:
        raise ValueError('区域太小，计算采样间距不足 0.0001 m，请扩大区域或减少点数。')
    if spacing_m is not None:nx=axis_count(width_requested,h)
    if not inset:ny=axis_count(height_requested,h)
    nx,ny=count(nx),count(ny)
    if nx*ny>4000000:
        raise ValueError('总高程点数不超过 4,000,000；请增大间距或缩小区域。')
    width,height=(nx-1)*h,(ny-1)*h
    footprint_width,footprint_height=(nx-int(endpoint))*h,(ny-int(endpoint))*h
    x0=(xmin+xmax-footprint_width/dem.xy_factor)/2 if inset else xmin
    y0=(ymin+ymax-footprint_height/dem.xy_factor)/2 if inset else ymin
    return dict(nx=nx,ny=ny,spacing_m=h,origin_x_m=x0*dem.xy_factor,origin_y_m=y0*dem.xy_factor,
                width_m=width,height_m=height,requested_width_m=width_requested,requested_height_m=height_requested,
                requested_roi_source=bounds.tolist(),
                sampled_roi_source=[x0,x0+width/dem.xy_factor,y0,y0+height/dem.xy_factor],
                extent_policy='centred inscribed footprint' if inset else 'exact ROI; samples anchored at minimum X/Y',
                endpoint_included=bool(endpoint),
                sampling_convention='L/(N-1); boundary included only when divisible by h' if endpoint else 'L/N; upper boundary excluded')


def sample_surface_grid(dem, roi, nx=300, ny=None, method='bilinear', *, spacing_m=None, endpoint=False):
    """Sample absolute elevations with shared X/Y spacing and explicit endpoints."""
    geometry=surface_grid_geometry(dem,roi,nx,ny,spacing_m=spacing_m,endpoint=endpoint)
    nx,ny,h=geometry['nx'],geometry['ny'],geometry['spacing_m']
    x0,y0=geometry['origin_x_m']/dem.xy_factor,geometry['origin_y_m']/dem.xy_factor
    x=x0+np.arange(nx)*h/dem.xy_factor
    y=y0+np.arange(ny)*h/dem.xy_factor
    xx,yy=np.meshgrid(x,y)
    values=dem.sample(xx.ravel(),yy.ravel(),method=method).reshape(ny,nx)
    if not np.isfinite(values).all():
        raise ValueError('二维区域包含 NoData 或超界高程。请移动 / 缩小区域，不会自动填零或只保留某一条有效剖面。')
    info=dict(format='VTK_XML_SURFACE',version=1,**geometry,
              datum_elevation_m=float(values.mean()),
              row_order='Y increasing; first row is minimum Y (south)',column_order='X increasing',
              elevation_range_m=[float(values.min()),float(values.max())],interpolation=method,
              height_processing='source metre elevations; no smoothing or shift',model_z='datum - elevation; Z positive down',
              note='Output sampling does not improve native DEM resolution.',dem=dem.info())
    return values,info


def surface_grid_stem(info):
    """The name is descriptive; the VTK file retains full spacing precision."""
    return f'surface_grid_{info["nx"]}x{info["ny"]}_h{info["spacing_m"]:.12g}m'


def write_surface_grid(values,info,folder,formats=('vti','vts')):
    formats=(formats,) if isinstance(formats,str) else tuple(formats)
    if not formats or len(set(formats))!=len(formats) or any(f not in ('vti','vts') for f in formats):
        raise ValueError('地表数据只支持 vti、vts 或两者同时导出。')
    nx,ny=info['nx'],info['ny']
    if not (2<=nx<=4096 and 2<=ny<=4096 and nx*ny<=4000000) or np.shape(values)!=(ny,nx) or not np.isfinite(values).all() or np.max(np.abs(values))>1e7:
        raise ValueError('区域数组形状或高程值无效。')
    h,x0,y0,datum=(float(info[k]) for k in ('spacing_m','origin_x_m','origin_y_m','datum_elevation_m'))
    if not np.isfinite([h,x0,y0,datum]).all() or h<=0:raise ValueError('间距、坐标原点或高程基准无效。')
    folder=Path(folder);folder.mkdir(parents=True,exist_ok=True)
    stem=surface_grid_stem(info)
    extent=f'0 {nx-1} 0 {ny-1} 0 0'
    for suffix in formats:
        kind='ImageData' if suffix=='vti' else 'StructuredGrid'
        with (folder/f'{stem}.{suffix}').open('w',encoding='utf-8',newline='\n') as stream:
            stream.write(f'<?xml version="1.0"?>\n<VTKFile type="{kind}" version="1.0" byte_order="LittleEndian">\n')
            attrs=f' Origin="{x0:.17g} {y0:.17g} 0" Spacing="{h:.17g} {h:.17g} 1"' if suffix=='vti' else ''
            stream.write(f'<{kind} WholeExtent="{extent}"{attrs}>\n<FieldData>\n')
            for name,value in (('ElevationDatum_m',datum),('HorizontalSpacing_m',h),('HorizontalUnitMetres',1),('ElevationUnitMetres',1),('ElevationPositiveUp',1)):
                stream.write(f'<DataArray type="Float64" Name="{name}" NumberOfComponents="1" NumberOfTuples="1" format="ascii">{value:.17g}</DataArray>\n')
            stream.write(f'</FieldData>\n<Piece Extent="{extent}">\n<PointData Scalars="Elevation">\n'
                         '<DataArray type="Float64" Name="Elevation" NumberOfComponents="1" format="ascii">\n')
            np.savetxt(stream,values,fmt='%.17g')
            stream.write('</DataArray>\n</PointData>\n<CellData/>\n')
            if suffix=='vts':
                stream.write('<Points>\n<DataArray type="Float64" NumberOfComponents="3" format="ascii">\n')
                x=x0+np.arange(nx)*h
                for j in range(ny):np.savetxt(stream,np.column_stack((x,np.full(nx,y0+j*h),values[j])),fmt='%.17g')
                stream.write('</DataArray>\n</Points>\n')
            stream.write(f'</Piece>\n</{kind}>\n</VTKFile>\n')
    np.savetxt(folder/f'{stem}_elevations.csv',values,delimiter=',',fmt='%.17g')
    saved=dict(info,format='VTK_XML_SURFACE',vtk_encoding='ascii Float64',
               vtk_files=[f'{stem}.{f}' for f in formats],filename_stem=stem,
               vti_geometry='flat XY image, Elevation point scalar; Warp By Scalar gives actual terrain',
               vts_geometry='explicit source X/Y and elevation-positive-up Z; one Z plane')
    (folder/f'{stem}_metadata.json').write_text(json.dumps(saved,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')


def _write_surface_grid_map(values,info,folder):
    import matplotlib.pyplot as plt
    fig,ax=plt.subplots(figsize=_dem_detail_figsize((0,info['width_m'],0,info['height_m'])),layout='constrained')
    try:
        h=info['spacing_m']
        x=(np.arange(info['nx']+1)-.5)*h
        y=(np.arange(info['ny']+1)-.5)*h
        image=ax.pcolormesh(x,y,values,cmap='jet',shading='flat',rasterized=True)
        ax.set(xlim=(0,info['width_m']),ylim=(0,info['height_m']),aspect='equal',
               xlabel='Local X (m)',ylabel='Local Y (m)',title=f"{info['nx']} x {info['ny']} surface grid")
        _dem_detail_colorbar(fig,image,ax)
        save_publication_figure(fig,folder,surface_grid_stem(info)+'_map')
    finally:
        plt.close(fig)


def export_surface_grid(dem,roi,output_dir=None,nx=300,ny=None,method='bilinear',*,spacing_m=None,endpoint=False,formats=('vti','vts')):
    values,info=sample_surface_grid(dem,roi,nx,ny,method,spacing_m=spacing_m,endpoint=endpoint)
    parent=Path(output_dir or dem.output_dir)
    name=datetime.now().strftime('region_%Y%m%d_%H%M%S_%f')
    pending=parent/(name+'.incomplete');final=parent/name
    try:
        write_surface_grid(values,info,pending,formats=formats)
        _write_surface_grid_map(values,info,pending)
        pending.rename(final)
        return final
    except Exception as exc:
        if pending.exists():(pending/'EXPORT_ERROR.txt').write_text(str(exc),encoding='utf-8')
        raise


def _export_profile_files(dem, table, metadata, output_dir=None, roi=None, *, height_view='absolute', annotate=True,
                          zoom_range_m=None, map_figure=None, map_view_bounds=None, folder=None,
                          viewport_figure=None, map_scope=None, surface_grid_shape=None):
    if len(table) < 2 or not np.isfinite(table[['x_m','elevation_m','dem_x','dem_y']]).all().all():
        raise ValueError('Export refused: selected profile includes NoData/out-of-bounds samples. Shorten the range or explicitly keep the longest valid segment.')
    if not (np.diff(table.x_m) > 0).all():
        raise ValueError('Export distances must increase strictly.')
    reference = profile_plot_reference(table, height_view)
    region_data=None
    if surface_grid_shape is not None:
        region_data=sample_surface_grid(dem,roi,*surface_grid_shape,method=metadata.get('interpolation','bilinear'))
    if zoom_range_m is not None:
        profile_plot_window(table,zoom_range_m)  # Validate before creating any output.
    folder = Path(folder) if folder is not None else Path(output_dir or dem.output_dir)/datetime.now().strftime('profile_%Y%m%d_%H%M%S_%f')
    folder.mkdir(parents=True,exist_ok=False)
    if region_data is not None:
        write_surface_grid(*region_data,folder)
        _write_surface_grid_map(*region_data,folder)
        metadata=dict(metadata,surface_grid_3d=region_data[1])
    table[['x_m','elevation_m']].to_csv(folder/'surface_profile.csv',index=False,float_format='%.9f')
    table.to_csv(folder/'profile_with_coordinates.csv',index=False,float_format='%.9f')
    metadata = dict(metadata, exported_utc=datetime.now(timezone.utc).isoformat(),samples=len(table),
                    output_length_m=float(table.x_m.iloc[-1]), elevation_range_m=[float(table.elevation_m.min()),float(table.elevation_m.max())],
                    roi_dem_coordinates=list(roi) if roi is not None else None,
                    profile_plot=dict(height_view=height_view, reference_elevation_m=reference,
                                      annotate_extrema=bool(annotate), csv_heights='unchanged source elevations',
                                      zoom_distance_range_m=list(map(float,zoom_range_m)) if zoom_range_m is not None else None,
                                      zoom_boundary_interpolation='linear between adjacent valid samples only',
                                      zoom_reference='same as full profile; horizontal distances are not reset'))
    (folder/'selection.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
    import matplotlib.pyplot as plt
    figure,ax = plt.subplots(figsize=(7.2,2.8),layout='constrained')
    plot_profile(table,ax,height_view=height_view,annotate=annotate,spacing_m=metadata.get('requested_spacing_m'))
    save_publication_figure(figure,folder,'profile')
    plt.close(figure)
    if zoom_range_m is not None:
        figure,ax = plt.subplots(figsize=(7.2,2.8),layout='constrained')
        plot_profile(table,ax,height_view=height_view,annotate=annotate,
                     spacing_m=metadata.get('requested_spacing_m'),distance_range_m=zoom_range_m)
        save_publication_figure(figure,folder,'profile_zoom')
        plt.close(figure)
    figure,ax = plt.subplots(figsize=(7.2,5),layout='constrained')
    image = plot_map(dem,ax,table,roi,max_size=2000)
    if map_view_bounds is not None:
        from matplotlib.patches import Rectangle
        a,b,c,d=map_view_bounds
        ax.add_patch(Rectangle((a,c),b-a,d-c,fill=False,edgecolor='#252525',lw=.7,ls='--'))
    figure.colorbar(image,ax=ax,label='Elevation (m)')
    ax.set_title('(a) DEM overview')
    save_publication_figure(figure,folder,'dem_with_profile')
    color_limits=image.get_clim()
    plt.close(figure)
    bounds=map_view_bounds if map_view_bounds is not None else roi
    if map_figure is not None and map_view_bounds is not None:
        save_publication_figure(map_figure,folder,'dem_zoom')
        metadata['dem_map_zoom']=dict(bounds_dem_coordinates=list(map(float,bounds)),
            source='detached snapshot of interactive map data',scope=map_scope,
            color_scale='unchanged from interactive figure',capture='Agg figure; live canvas untouched')
    elif bounds is not None:
        metadata['dem_map_zoom']=_write_dem_zoom(dem,bounds,folder,table,color_limits)
        metadata['dem_map_zoom']['source']='selected region; interactive viewport unavailable'
    if viewport_figure is not None:
        save_publication_figure(viewport_figure,folder,'dem_view')
        view_ax=viewport_figure.axes[0]
        metadata['dem_current_view']=dict(bounds_dem_coordinates=list(map(float,(*view_ax.get_xlim(),*view_ax.get_ylim()))),
                                         source='detached snapshot of current viewport')
    metadata['figure_export']=dict(style='publication',png_dpi=600,formats=['png','pdf','svg'])
    (folder/'README.txt').write_text(
        'DEM / profile results\n'
        'dem_with_profile: full DEM overview; dashed black box marks exported viewport when available.\n'
        'dem_zoom: local DEM area resolved by export scope (see selection.json).\n'
        'dem_view: exact current viewport, when interactive map is available. Export uses a detached canvas.\n'
        'profile: full sampled elevation profile. profile_zoom: optional distance-window detail.\n'
        'Each figure is supplied as 600 dpi PNG, PDF and SVG. Raster map content remains raster in vector files.\n'
        'surface_profile.csv: simulation input with unchanged source elevations.\n'
        'profile_with_coordinates.csv: full sampling coordinates. selection.json: provenance and view bounds.\n'
        'surface_grid_*_h*m.vti / .vts, elevations CSV and metadata JSON: optional 2D surface array.\n'
        'surface_grid_map: sampled region map; actual inscribed region is recorded separately from requested ROI.\n',encoding='utf-8')
    metadata['files']=sorted(p.name for p in folder.iterdir() if p.is_file())
    (folder/'selection.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
    return folder


def export_profile(dem, table, metadata, output_dir=None, roi=None, **options):
    """Publish a complete bundle; failed exports stay explicitly incomplete."""
    # Support direct callers without ever passing a live widget into savefig.
    source=options.pop('map_figure',None)
    viewport=options.pop('viewport_figure',None)
    local=None
    if source is not None:
        local=snapshot_map_figure(source,options.get('map_view_bounds'))
    if viewport is not None:
        viewport=snapshot_map_figure(viewport)
    name=datetime.now().strftime('profile_%Y%m%d_%H%M%S_%f')
    parent=Path(output_dir or dem.output_dir)
    pending=parent/(name+'.incomplete')
    final=parent/name
    try:
        _export_profile_files(dem,table,metadata,output_dir,roi,folder=pending,
                              map_figure=local,viewport_figure=viewport,**options)
        record=json.loads((pending/'selection.json').read_text(encoding='utf-8'))
        record['export_status']='complete'
        (pending/'selection.json').write_text(json.dumps(record,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
        pending.rename(final)
        return final
    except Exception as exc:
        if pending.exists():
            (pending/'EXPORT_ERROR.txt').write_text('Incomplete export; not a finished result.\n'+str(exc),encoding='utf-8')
            raise RuntimeError(f'导出未完成；中间结果及错误说明保存在 {pending}。原因：{exc}') from exc
        raise
    finally:
        for figure in (local,viewport):
            if figure is not None:
                figure.clear()
