"""Publish unmodified, native-resolution solver screenshots after visual review.

Run the application --capture-workflow, --capture-2d and --smoke-home first.
Raw frames stay in ../out; only verified publication PNG/TSV enter docs/images.
"""
from pathlib import Path
import csv, shutil, hashlib, json
from PIL import Image
r=Path(__file__).resolve().parents[1]; out=r.parent/'out/screenshots'; shots=r/'docs/images'
published=[]
for name in ['curvilinear-workflow','two-d-workflow']:
    source=out/name; target=shots if name=='curvilinear-workflow' else shots/'two-d'
    target.mkdir(parents=True,exist_ok=True)
    rows=list(csv.DictReader((source/'capture.tsv').open(encoding='utf-8'),delimiter='\t'))
    assert len(rows)==(20 if name=='curvilinear-workflow' else 13)
    for row in rows:
        assert row['capture_date']=='2026-10-27' and row['theme']=='Red Yellow Orange'
        assert row['record_palette']=='0' and row['record_style']=='3' and float(row['record_gain'])==1
        if int(row['step'])>0: assert float(row['record_height'])>=450, row['image']
        raw=source/(row['image']+'.ppm'); dest=target/(row['image']+'.png')
        with Image.open(raw) as im:
            assert im.size==(2560,1494)
            im.save(dest,optimize=True)
            with Image.open(dest) as saved: assert saved.size==im.size and saved.tobytes()==im.tobytes()
        published.append(dict(row,dimension='3D' if name=='curvilinear-workflow' else '2D',path=dest.relative_to(r/'docs').as_posix(),sha256=hashlib.sha256(dest.read_bytes()).hexdigest()))
    shutil.copy2(source/'capture.tsv',target/'capture.tsv')
with Image.open(out/'two-d/home-maximized.ppm') as im:
    assert im.size==(2560,1494);im.save(shots/'two-d/home.png',optimize=True)
print('Published 20 3D + 13 2D + 1 home full-size PNG images, lossless and uncropped.')

published.append(dict(image='home',dimension='Home',path='images/two-d/home.png',width='2560',height='1494',theme='Red Yellow Orange',capture_date='2026-10-27',sha256=hashlib.sha256((shots/'two-d/home.png').read_bytes()).hexdigest()))
(shots/'publication.json').write_text(json.dumps({'date':'2026-10-27','theme':'Red Yellow Orange','images':published},ensure_ascii=False,indent=2),encoding='utf-8')
labels3d=['建模与三维曲网格','震源与采样设置','接收测线配置','运行参数与时间步','0.10 s波场','0.25 s波场','0.40 s波场','原始地震道集','同一记录的AGC预览','1倍增益单道三分量','网格与吸收层显示','快捷键帮助','模型与记录导出','分层地层模型','30 Hz加密网格','50 Hz加密网格','60 Hz加密网格','1024位置接收面阵','默认DEM的turbo空间地表','默认DEM的turbo俯视']
labels2d={'model':'二维建模','workbench':'二维运行工作台','completed':'二维完成与完整接收记录','model-io':'模型保存与载入','export':'波场与CSV导出','segy':'SEG-Y导出','help':'快捷键说明','display':'图层与配色','record-options':'记录显示设置','layers':'分层介质表','terrain':'外部高程剖面','source-explosion':'爆炸矩源与波场','elevation-examples':'高程示例预设'}
gallery=['# 当前完整窗口截图','', '全部界面图于2026-10-27从当前Release重新采集，统一红黄橙主题，2560×1494原生完整窗口，无裁剪、缩放或重着色。二维参数、波场与道集并列；三维记录页扩大高度。波场及记录使用程序原有配色与叠加样式，不使用gray；单道三分量为1倍增益，地表高程使用turbo。','', '这些界面图用于操作说明；静态地形图不证明传播精度，AGC与自动色限不能用于跨图绝对振幅比较。性能曲线及数值对照图具有各自独立的原始数据和实验日期。','']
for i,row in enumerate(published,1):
 label=labels3d[i-1] if row['dimension']=='3D' else labels2d.get(row['image'],'二维/三维主页')
 gallery += [f"## {i}. {row['dimension']} · {label}",'',f"{row['width']}×{row['height']} | 红黄橙 | {row['capture_date']}"+(' | '+row['backend']+' | t='+f"{float(row['time_s']):.6f} s" if row.get('backend') else ''),'',f"![{label}]({row['path']})",'']
(r/'docs/SCREENSHOT_GALLERY.md').write_text('\n'.join(gallery),encoding='utf-8')
