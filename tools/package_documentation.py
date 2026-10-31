"""Losslessly prepare native screenshots and package Markdown for local/Jekyll/Hexo.

All generated files stay in outer out/. No deployment or user site modification.
"""
import csv
import hashlib
import html
import json
import re
import shutil
import os
import zipfile
from pathlib import Path
from PIL import Image
from project_version import project_version

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT.parent / 'out'
SHOTS = ROOT / 'docs/images'
DOCS = OUTPUT / 'documentation/SeisWaveLab_Curvilinear'
BLOG = OUTPUT / 'blog/SeisWaveLab_Curvilinear'
TOKEN = re.compile(r'(!?)\[([^\]]*)\]\(([^)]+)\)')
CODE = re.compile(r'(```[\s\S]*?```|`[^`\n]*`)')
def references(text):
    return TOKEN.findall(CODE.sub('',text))
TITLE = 'SeisWave Lab：曲网格二维与三维弹性波模拟的理论、实现与应用'
UPDATED = '2026-10-31'
CAPTURE_DATE = '2026-10-27'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def archive(folder, destination, files):
    """Publish only explicitly selected assets, never stale generated reports."""
    with zipfile.ZipFile(destination,'w',zipfile.ZIP_DEFLATED) as z:
        for relative in sorted(set(files)):
            p=(folder/relative).resolve()
            p.relative_to(folder.resolve())
            assert p.is_file(),p
            z.write(p,relative)

def retain_legacy_reports(folder):
    """Move known former publication intermediates to a separate local archive."""
    out=OUTPUT.resolve()
    for name in ['capture-manifest.json','phase_comparison.csv','summary.json',
                 'VERIFICATION_0_16.md','reference','data','package-validation.json']:
        source=(folder/name).resolve()
        source.relative_to(out)
        if not source.exists(): continue
        target=(out/'documentation-history/2026-10-17'/source.relative_to(out)).resolve()
        target.relative_to(out)
        if target.exists():
            number=1
            while target.with_name(target.name+f'.{number}').exists(): number+=1
            target=target.with_name(target.name+f'.{number}')
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.move(str(source),str(target))

def main():
    github=BLOG/'github-pages';hexo=BLOG/'hexo'
    github_assets=github/'assets/seis3d-curvilinear'
    hexo_post=hexo/'source/_posts/seis3d-curvilinear'
    # Current validation/data are now publication assets, not legacy staging.
    DOCS.mkdir(parents=True,exist_ok=True)
    (DOCS/'images').mkdir(exist_ok=True)
    rows=list(csv.DictReader((SHOTS/'capture.tsv').open(encoding='utf-8'),delimiter='\t'))
    assert len(rows)==20, 'Expected twenty current workflow captures including Turbo DEM'
    for row in rows:
        assert int(row['samples'])==int(row['step'])+1,'Receiver sample count differs from accepted-step timeline'
        assert abs(float(row['time_s'])-int(row['step'])*float(row['dt_s']))<1e-12,'Capture time does not match actual dt'
        png=SHOTS/(row['image']+'.png')
        with Image.open(png) as im:
            im.verify()
        with Image.open(png) as im:
            recorded=[int(row['width']),int(row['height'])]
            row.update(recorded_size=recorded,published_size=list(im.size),metadata_size_matches=list(im.size)==recorded)
            pixels=im.convert('RGB').tobytes()
        row.update(png=png.name,sha256=digest(png),pixel_sha256=hashlib.sha256(pixels).hexdigest())
        shutil.copy2(png,DOCS/'images'/png.name)
    metadata=dict(version=project_version(),date=UPDATED,recorded_three_d_capture_date=CAPTURE_DATE,recorded_two_d_capture_date=CAPTURE_DATE,theme='Red Yellow Orange',pixel_processing='Fresh full-window native framebuffer captures; lossless PNG; no crop, resize or recolor',captures=rows)
    shutil.copy2(SHOTS/'dispersion-comparison.png',DOCS/'images/dispersion-comparison.png')

    selected_docs={p.name for p in (ROOT/'docs').glob('*.md') if p.name!='README.md'}
    linked_files=set()
    pending=[]
    converted=set()
    def mapped(resolved):
        resolved.relative_to(ROOT)
        if resolved.is_relative_to(SHOTS):
            parts=resolved.relative_to(SHOTS).parts
            return 'images/'+'-'.join(parts)
        if resolved.is_relative_to(ROOT/'docs'):
            rel=resolved.relative_to(ROOT/'docs').as_posix()
            return 'REFERENCE_INDEX.md' if rel=='README.md' else rel
        if resolved.name in {'LICENSE','THIRD_PARTY_NOTICES.md'} and resolved.parent==ROOT:
            return resolved.name
        return 'reference/'+resolved.relative_to(ROOT).as_posix()
    def convert(text,source):
        def link(match):
            bang,label,target=match.groups()
            if target.startswith(('https:','http:','#','mailto:')): return match.group()
            path,_,anchor=target.partition('#');resolved=(source.parent/path).resolve()
            # Tool READMEs may link to local experiments outside the standalone
            # repository. Do not pull user datasets/results into documentation.
            if resolved.is_relative_to(OUTPUT):
                return f'{label}（工作区本地资料，未随离线包分发）'
            if not resolved.is_file(): raise ValueError(f'Missing local reference: {source}: {target}')
            dest=mapped(resolved)
            linked_files.add(dest)
            if resolved.suffix.lower()=='.md':
                if resolved not in converted: pending.append(resolved)
            else:
                (DOCS/dest).parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(resolved,DOCS/dest)
            dest=Path(os.path.relpath(DOCS/dest,(DOCS/mapped(source)).parent)).as_posix()
            if anchor: dest+='#'+anchor
            return f'{bang}[{label}]({dest})'
        return ''.join(chunk if i%2 else TOKEN.sub(link,chunk)
                       for i,chunk in enumerate(CODE.split(text)))

    source=ROOT/'docs/PROJECT_MANUAL.md'
    converted.add(source)
    source_text=source.read_text(encoding='utf-8')
    body=convert(source_text,source)
    replaced=[row['image'] for row in rows if not row['metadata_size_matches']]
    if replaced and '图像资料说明：' not in source_text:
        notice='> 图像资料说明：'+', '.join(replaced)+' 的当前 PNG 尺寸与旧采集清单不一致；保留原图，不将旧清单中的采集日期或数值条件视为这些图片的已核验条件。其余截图也不作为本次界面更新的重新采集证据。\n\n'
        body=notice+body
    assert re.findall(r'^## (.+)$',body,re.M)==['1 背景','2 原理','3 实现过程','4 界面介绍','5 附录']
    (DOCS/'PROJECT_MANUAL.md').write_text(body,encoding='utf-8')
    for name in sorted(selected_docs-{'PROJECT_MANUAL.md'}):
        page=ROOT/'docs'/name
        converted.add(page)
        (DOCS/name).write_text(convert(page.read_text(encoding='utf-8'),page),encoding='utf-8')
    while pending:
        page=pending.pop()
        if page in converted: continue
        converted.add(page)
        target=DOCS/mapped(page)
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_text(convert(page.read_text(encoding='utf-8'),page),encoding='utf-8')
    # Preserve the complete small published evidence set and linked notebook dependencies.
    for base in [ROOT/'docs/data/performance-2026-10-19',ROOT/'tools/dem_profiles']:
        for resource in base.iterdir():
            if not resource.is_file() or resource.suffix=='.pyc': continue
            dest=mapped(resource)
            if resource.suffix=='.md' and resource in converted: continue
            (DOCS/dest).parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(resource,DOCS/dest)
            linked_files.add(dest)
    rows2d=list(csv.DictReader((SHOTS/'two-d/capture.tsv').open(encoding='utf-8'),delimiter='\t'))
    assert len(rows2d)==13
    for row in rows2d:
        png=SHOTS/'two-d'/(row['image']+'.png')
        with Image.open(png) as im:
            im.verify()
        with Image.open(png) as im:
            recorded=[int(row['width']),int(row['height'])]
            row.update(recorded_size=recorded,published_size=list(im.size),metadata_size_matches=list(im.size)==recorded)
            pixel_hash=hashlib.sha256(im.convert('RGB').tobytes()).hexdigest()
        shutil.copy2(png,DOCS/'images'/('two-d-'+png.name))
        row.update(png='two-d-'+png.name,sha256=digest(png),pixel_sha256=pixel_hash)
    metadata['two_d_captures']=rows2d
    metadata['two_d_theme']='Red Yellow Orange'
    with Image.open(SHOTS/'two-d/home.png') as image:home_size=list(image.size)
    metadata['home_capture']={'png':'two-d-home.png','published_size':home_size,'sha256':digest(SHOTS/'two-d/home.png'),'content':'Program-drawn functional illustration; no solver state'}
    metadata['total_native_screenshots']=len(rows)+len(rows2d)+1
    (DOCS.parent/'capture-manifest.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    gallery=['# 界面与模拟过程图集' ,'', '34张2026-10-27重新采集的完整窗口界面图，统一红黄橙主题，2560×1494原生分辨率，无裁剪、无拉伸。20张三维（含默认DEM的turbo地表及俯视）、13张二维流程，另有主页。二维采用参数/波场/记录并列，三维记录页扩大道集高度。算例说明见[主文附录 E](PROJECT_MANUAL.md)。不同会话、自动色标或 AGC 视图不能直接比较绝对幅度，初始界面不构成动态计算精度证据。','']
    image_labels={Path(target).stem:label for bang,label,target in TOKEN.findall(source_text) if bang}
    for number,row in enumerate(rows,1):
        label=image_labels[row['image']]
        if not row['metadata_size_matches']:
            gallery += [f"## {number}. {label}",'',f"当前图片 {row['published_size']}，与旧采集清单尺寸不一致；该图的采集日期和模型条件尚未重新核验，不附用旧数值条件。",'',f"![{label}](images/{row['png']})",'']
            continue
        gallery += [f"## {number}. {label}",'',f"{row['backend']} | {row['nx']}×{row['ny']}×{row['nz']} | h={row['spacing_m']} m | f₀={row['f0_hz']} Hz | 第 {row['step']} 步 | t={float(row['time_s']):.6f} s | dt={float(row['dt_s'])*1000:.6f} ms | 参考 PPW={float(row['ppw']):.3f} | {row['receivers']} 位置×{row['samples']} 样本",'',f"![{label}](images/{row['png']})",'']
    labels2d={'model':'二维建模','workbench':'参数标签与运行进度','completed':'模拟完成与接收记录','model-io':'模型保存与载入','export':'波场分量导出','segy':'SEG-Y 兼容导出','help':'横向快捷键帮助','display':'独立图层与配色','record-options':'记录显示参数','layers':'分层介质编辑','terrain':'高程剖面导入','source-explosion':'爆炸矩源波场','elevation-examples':'山丘与沟谷示例'}
    for number,row in enumerate(rows2d,len(rows)+1):
        label=labels2d[row['image']]
        if not row['metadata_size_matches']:
            gallery += [f"## {number}. {label}",'',"当前图片与旧采集清单尺寸不一致；该图的采集条件尚未重新核验。",'',f"![{label}](images/{row['png']})",'']
            continue
        gallery += [f"## {number}. {label}",'',f"{row['backend']} | {row['nx']}×{row['nz']} | h={row['spacing_m']} m | f₀={row['f0_hz']} Hz | 第 {row['step']} 步 | t={float(row['time_s']):.6f} s | dt={float(row['dt_s'])*1000:.6f} ms | 记录间隔={float(row['record_dt_s'])*1000:.3f} ms",'',f"![{label}](images/{row['png']})",'']
    gallery += [f'## {len(rows)+len(rows2d)+1}. 曲面二维与三维入口','','![主页，程序绘制的功能示意](images/two-d-home.png)','']
    (DOCS/'SCREENSHOT_GALLERY.md').write_text('\n'.join(gallery),encoding='utf-8')
    cards=[]
    for row in rows+rows2d:
        label=image_labels.get(row['image'],labels2d.get(row['image'],row['image']))
        cards.append(f'<section><h2>{html.escape(label)}</h2><p>{row["published_size"][0]}×{row["published_size"][1]} · 红黄橙 · {CAPTURE_DATE}</p><a href="images/{row["png"]}"><img loading="lazy" src="images/{row["png"]}" alt="{html.escape(label)}"></a></section>')
    cards.append('<section><h2>二维 / 三维主页</h2><a href="images/two-d-home.png"><img loading="lazy" src="images/two-d-home.png" alt="主页"></a></section>')
    gallery_html='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>SeisWave Lab 完整窗口截图</title><style>body{font:16px "Microsoft YaHei",sans-serif;margin:0;background:#fff5e7;color:#382118}header{background:linear-gradient(120deg,#a52218,#e57724);color:white;padding:30px}main{margin:auto;max-width:1900px;padding:20px}section{margin:0 0 28px;background:white;border:1px solid #efd3ae;padding:18px}h2{font-size:22px;color:#a52218}img{width:100%;height:auto}p{line-height:1.8}</style><header><h1>SeisWave Lab · 完整窗口图集</h1><p>2026-10-27重新采集34张界面图，原生2560×1494，无裁切与拉伸。红黄橙主题；波场与接收记录沿用原有彩色显示，地表使用turbo。点击图片可查看完整原图。</p></header><main>'+''.join(cards)+'</main></html>'
    (DOCS/'SCREENSHOT_GALLERY.html').write_text(gallery_html,encoding='utf-8')

    for name in ['LICENSE','THIRD_PARTY_NOTICES.md']: shutil.copy2(ROOT/name,DOCS/name)
    (DOCS/'README.md').write_text('# SeisWave Lab 图文说明\n\n[用户手册](USER_GUIDE.md) 提供实际操作与数据复现；[完整说明](PROJECT_MANUAL.md) 按背景、原理、实现过程、界面介绍和附录组织，包含数学推导、算例条件、数据定义与参考文献。[采集生产应用分析](ACQUISITION_PRODUCTION_VALUE.md) 说明黄土塬试验用途与能力边界；[最新二维精度性能](PERFORMANCE_PRECISION_2D.md)、[历史性能](PERFORMANCE_BENCHMARK.md)和[当前验证](NUMERICS_AND_VALIDATION.md) 提供计时及证据。[界面与模拟过程图集](SCREENSHOT_GALLERY.md) 收录34张2026-10-27新采集的红黄橙主题完整窗口图，正文另有内点频散分析图，图像均在 images 目录。\n\n本文区分数学模型、当前实现和已有证据范围，不以运行完成或后端一致代替物理精度验证。普通 Markdown 阅读器需启用数学公式渲染。软件许可和来源见随附 LICENSE 与 THIRD_PARTY_NOTICES.md。\n',encoding='utf-8')

    doc_files=sorted(selected_docs)+['SCREENSHOT_GALLERY.md','SCREENSHOT_GALLERY.html','README.md','LICENSE','THIRD_PARTY_NOTICES.md']+sorted(linked_files)
    for resource in sorted((ROOT/'resources/examples/elevation').rglob('*')):
        if resource.suffix.lower() not in {'.csv','.md'}: continue
        relative='examples/elevation/'+resource.relative_to(ROOT/'resources/examples/elevation').as_posix()
        (DOCS/relative).parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(resource,DOCS/relative)
        doc_files.append(relative)
    doc_files += ['images/'+row['png'] for row in rows+rows2d]+['images/dispersion-comparison.png']
    # Additional published UI images (e.g. the 2D/3D home screen) are not part
    # of the original 18-shot 3D numerical capture series or its metadata.
    for page in selected_docs|{'SCREENSHOT_GALLERY.md'}:
        for bang,label,target in TOKEN.findall((DOCS/page).read_text(encoding='utf-8')):
            if bang and target.startswith('images/') and target not in doc_files:
                doc_files.append(target)
    (github/'_posts').mkdir(parents=True,exist_ok=True)
    hexo_post.parent.mkdir(parents=True,exist_ok=True)
    for destination in [github_assets,hexo_post]:
        for relative in doc_files:
            target=destination/relative
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(DOCS/relative,target)
    # Bare filenames resolve inside Hexo's same-name post asset folder.
    for relative in doc_files:
        if relative.startswith('images/'):
            shutil.copy2(DOCS/relative,hexo_post/Path(relative).name)
    header='---\ntitle: '+json.dumps(TITLE,ensure_ascii=False)+'\ndate: 2026-10-16\nupdated: '+UPDATED+'\ntags: [Seismology, Curvilinear Grid, Elastic Wave, CUDA, Finite Difference]\ncategories: [计算地球物理, 数值模拟]\nmathjax: true\ntoc: true\n'
    # No guessed remote URL or domain: relative_url supports project Pages baseurl.
    def github_link(m):
        bang,label,target=m.groups()
        if target.startswith(('http:','https:','#','mailto:')): return m.group()
        return f"{bang}[{label}]({{{{ '/assets/seis3d-curvilinear/{target}' | relative_url }}}})"
    github_body=TOKEN.sub(github_link,body)
    (github/'_posts/2026-10-16-seis3d-curvilinear.md').write_text(header+'layout: post\n---\n\n'+github_body,encoding='utf-8')
    def hexo_link(m):
        bang,label,target=m.groups()
        if target.startswith(('http:','https:','#','mailto:')): return m.group()
        if bang: return f'![{label}]({Path(target).name})'
        return '{% asset_link '+target+' "'+label.replace('"','\\"')+'" %}'
    (hexo/'source/_posts/seis3d-curvilinear.md').write_text(header+'---\n\n'+TOKEN.sub(hexo_link,body),encoding='utf-8')
    (BLOG/'README.md').write_text('''# GitHub Pages / Hexo 发布包

两版均由同一份正文生成，依次为背景、原理、实现过程、界面介绍和附录。数学推导、算例条件、证据边界及参考文献一致，文章及新增界面说明更新于2026年10月31日，截图采集日期见各页说明和清单。附带最新验证汇总、性能报告、倍速曲线和60轮原始计时数据；不包含试跑、构建日志、EXE或计算中间场。

## GitHub Pages（Jekyll）

把 github-pages/_posts 的文章合并至站点 _posts，把 assets/seis3d-curvilinear 合并到站点同路径。文章使用 layout: post，站点应有该布局；也可改为现有主题的文章布局。图像使用 Jekyll relative_url 自动考虑 baseurl，无需猜测用户名或仓库名。正文采用 `$...$` 行内公式与 `$$` 独立公式，并使用 aligned 环境和公式编号；请在主题及 Markdown 渲染器中配置对应数学支持。mathjax: true 是主题元数据，不会自行加载脚本。不要把 Jekyll 版直接交给 Hexo 解析。

## Hexo

把 hexo/source/_posts 的文章和同名 seis3d-curvilinear 资源目录合并到站点 source/_posts。正文图片采用 `![说明](01-model.png)`，PNG 直接放在 seis3d-curvilinear 目录中。原 images 子目录仍供附带的离线说明使用，非图像附件链接保留 asset_link。

在 _config.yml 设置 post_asset_folder: true。使用 hexo-renderer-marked 时，同时配置 marked.prependRoot: true 和 marked.postAsset: true，使裸文件名解析到文章同名资源目录，兼顾正文及列表页。该配置依据 https://hexo.io/docs/asset-folders 。公式与 Jekyll 版采用相同行内及独立定界符，请启用支持这些定界符、aligned 环境和 tag 编号的 MathJax/KaTeX 配置及 Markdown 渲染器，避免普通 Markdown 先消耗公式反斜线；mathjax: true 不等于已经安装数学渲染插件。

若使用 NexT 和旧 kramed 导致公式不显示，可采用已验证的构建时 SVG 配置，见[数学渲染修复说明](hexo/source/_posts/seis3d-curvilinear/HEXO_MATH.md)。该方案使用 hexo-renderer-markdown-it 7.1.1 与 markdown-it-mathjax3 5.2.0，并关闭主题重复的前端数学引擎。

复制到现有站点即可本地预览；本工具不改站点配置、不覆盖主题，也不执行推送或发布。站点主题的最终数学/页面渲染需在该站点预览。已检查本包 Markdown 引用、图像存在性、PNG 像素摘要和原生分辨率。

## 无站点阅读

assets 下的 PROJECT_MANUAL.md / SCREENSHOT_GALLERY.md 为普通相对路径 Markdown。另有独立 SeisWaveLab_Curvilinear_Documentation.zip 文档包。图片含完整原图；不要对比自动色标的跨时刻绝对幅度。

官方依据：https://jekyllrb.com/docs/front-matter/ ，https://jekyllrb.com/docs/liquid/filters/ ，https://hexo.io/docs/asset-folders 。
''',encoding='utf-8')
    for p in [DOCS/f for f in doc_files if f.endswith('.md')]:
        for bang,label,target in references(p.read_text(encoding='utf-8')):
            if target.startswith(('https:','http:','#','mailto:')): continue
            assert (p.parent/target.split('#')[0]).is_file(),(p,target)
    assert github_body.count('| relative_url')==len([m for m in TOKEN.finditer(body) if not m[3].startswith(('http:','https:','#','mailto:'))])
    for path in re.findall(r"\{\{ '([^']+)' \| relative_url \}\}",github_body):
        assert (github/path.lstrip('/').split('#')[0]).is_file(),path
    hexo_text=(hexo/'source/_posts/seis3d-curvilinear.md').read_text(encoding='utf-8')
    for path in re.findall(r'\{% asset_(?:img|link) (\S+)',hexo_text):
        assert (hexo_post/path.split('#')[0]).is_file(),path
    for bang,label,path in TOKEN.findall(hexo_text):
        if bang and not path.startswith(('http:','https:')):
            assert Path(path).name==path and (hexo_post/path).is_file(),path
    assert body.count('$$')%2==0 and body.count('```')%2==0,'Unbalanced math/code delimiters'
    doc_files=sorted(set(doc_files))
    report=dict(updated=UPDATED,sections=5,appendices=8,published_screenshots=len(rows)+len(rows2d)+1,three_d=len(rows),two_d=len(rows2d),published_sizes=sorted(set(tuple(row['published_size']) for row in rows+rows2d)),metadata_size_mismatches=[row['png'] for row in rows+rows2d if not row['metadata_size_matches']],markdown_pages_checked=sum(f.endswith('.md') for f in doc_files),github_article='github-pages/_posts/2026-10-16-seis3d-curvilinear.md',hexo_article='hexo/source/_posts/seis3d-curvilinear.md',hexo_image_format='markdown-bare-filename',pixel_hashes_verified=True)
    report_path=OUTPUT/'maintenance/package-validation.json'
    report_path.parent.mkdir(parents=True,exist_ok=True)
    report_path.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    blog_files=['README.md','github-pages/_posts/2026-10-16-seis3d-curvilinear.md','hexo/source/_posts/seis3d-curvilinear.md']
    blog_files += ['github-pages/assets/seis3d-curvilinear/'+f for f in doc_files]
    blog_files += ['hexo/source/_posts/seis3d-curvilinear/'+f for f in doc_files]
    blog_files += ['hexo/source/_posts/seis3d-curvilinear/'+Path(f).name for f in doc_files if f.startswith('images/')]
    archive(DOCS,OUTPUT/'documentation/SeisWaveLab_Curvilinear_Documentation.zip',doc_files)
    archive(BLOG,OUTPUT/'blog/SeisWaveLab_Curvilinear_GitHub_Pages_Hexo.zip',blog_files)
    for package in [OUTPUT/'documentation/SeisWaveLab_Curvilinear_Documentation.zip',OUTPUT/'blog/SeisWaveLab_Curvilinear_GitHub_Pages_Hexo.zip']:
        with zipfile.ZipFile(package) as z: assert z.testzip() is None,package
    print(json.dumps(report,ensure_ascii=False,indent=2))

if __name__=='__main__': main()
