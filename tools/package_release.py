"""Stage/finalize a Windows release, keeping every generated file in outer out.

Uses the current working tree, never git archive of an older committed revision.
No compiler, driver, Python environment or private simulation data is redistributed.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from project_version import project_version

REPO = Path(__file__).resolve().parents[1]
OUT = REPO.parent / 'out'
PRODUCT = 'SeisWaveLab_Curvilinear'

def sha(path: Path) -> str:
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')

def copy(src: Path, dst: Path) -> None:
    if not src.is_file():
        raise FileNotFoundError(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)

def git(*args: str) -> str:
    return subprocess.check_output(['git', '-C', str(REPO), *args]).decode('utf-8').strip()

def pe_imports(path: Path) -> list[str]:
    """Read PE32/PE32+ import names without a third-party Python dependency."""
    b = path.read_bytes()
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    if b[pe:pe+4] != b'PE\0\0':
        raise ValueError(f'Not a PE file: {path}')
    sections, optional_size = struct.unpack_from('<H12xH', b, pe+6)
    optional = pe + 24
    magic = struct.unpack_from('<H', b, optional)[0]
    directory = optional + (112 if magic == 0x20b else 96)
    imports = struct.unpack_from('<I', b, directory+8)[0]
    table = optional + optional_size
    def offset(rva: int) -> int:
        for i in range(sections):
            size, virtual, raw_size, raw = struct.unpack_from('<IIII', b, table+40*i+8)
            if virtual <= rva < virtual+max(size, raw_size):
                return raw+rva-virtual
        raise ValueError(f'Unmapped RVA {rva} in {path}')
    names = []
    if imports:
        at = offset(imports)
        while any(struct.unpack_from('<IIIII', b, at)):
            name = offset(struct.unpack_from('<I', b, at+12)[0])
            names.append(b[name:b.index(0, name)].decode('ascii'))
            at += 20
    return names

def manifests(folder: Path) -> None:
    files = sorted(p for p in folder.rglob('*') if p.is_file() and p.name != 'FILE_MANIFEST.json' and 'userdata' not in p.relative_to(folder).parts and 'out' not in p.relative_to(folder).parts)
    write(folder/'FILE_MANIFEST.json', json.dumps([
        {'path': p.relative_to(folder).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)} for p in files
    ], ensure_ascii=False, indent=2))

def stage(args, root: Path, version: str) -> None:
    runtime_versions={}
    for executable in [PRODUCT,'SeisWaveLab_MASW']:
        actual=subprocess.check_output([str(OUT/'bin/Release'/(executable+'.exe')),'--version'],text=True).strip()
        if actual!=f'{executable} v{version}':
            raise RuntimeError(f'Rebuild required: VERSION={version}, executable reports {actual}')
        runtime_versions[executable]=actual
    root.mkdir(parents=True, exist_ok=False)
    portable = root/f'{PRODUCT}-v{version}-Windows-x64-Portable'
    source = root/f'{PRODUCT}-v{version}-Source'
    portable.mkdir(); source.mkdir()
    binary = OUT/'bin/Release'
    for name in [f'{PRODUCT}.exe', 'SeisWaveLab_MASW.exe', 'glfw3.dll', 'glew32.dll', 'cudart64_12.dll']:
        copy(binary/name, portable/name)
    for name in ['shaders', 'examples', 'licenses']:
        shutil.copytree(binary/name, portable/name)
    shutil.copytree(REPO/'docs', portable/'docs')
    for name in ['LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md']:
        copy(REPO/name, portable/name)
    redist = args.vc_redist_root
    for p in (redist/'x64/Microsoft.VC143.CRT').glob('*.dll'):
        copy(p, portable/p.name)
    copy(redist/'x64/Microsoft.VC143.OpenMP/vcomp140.dll', portable/'vcomp140.dll')
    copy(redist/'vc_redist.x64.exe', portable/'prerequisites/vc_redist.x64.exe')
    copy(args.cuda_root/'EULA.txt', portable/'licenses/NVIDIA_CUDA_EULA.txt')
    write(portable/'portable.flag', 'Portable distribution: write preferences and results to adjacent userdata.\n')
    for label, flag in [('Start', ''), ('Start-2D', '--2d'), ('Start-3D', '--3d')]:
        write(portable/(label+'.cmd'), f'@echo off\ncd /d "%~dp0"\nstart "" "%~dp0{PRODUCT}.exe" {flag}\n')
    write(portable/'README_使用说明.md', f'''# SeisWave Lab · Curvilinear v{version}

解压整个文件夹到可写目录，双击 Start.cmd 或 {PRODUCT}.exe。
Start-2D.cmd / Start-3D.cmd 直接进入相应工作区。

- Windows x64，需显卡驱动支持 OpenGL 4.3。CPU 计算不要求 NVIDIA 显卡。
- CUDA 加速需要兼容 NVIDIA 驱动及可用显存；无需安装 CUDA Toolkit。
- 已包含 GLFW、GLEW、CUDA runtime 和 Microsoft C++ / OpenMP app-local 运行库。
- 若系统仍提示运行库错误，可手动运行 prerequisites/vc_redist.x64.exe。
- 首次运行在旁边创建 userdata，界面偏好、默认导出与日志均在其中；不会使用开发机 D 盘路径。
- 文档入口 docs/USER_GUIDE.md；示例位于 examples/elevation；着色器在 shaders。
- 应用退出后可整体移动目录。不要只复制 EXE，也不要直接在 ZIP 预览中运行。
- 这是免安装便携包，不会注册 Windows 服务、修改 PATH 或安装显卡驱动。

对应完整源码请下载同一 Release 的 Source.zip。LICENSE 和 licenses 保留项目及第三方许可。
''')
    tracked = subprocess.check_output(['git', '-C', str(REPO), 'ls-files', '-z', '--cached', '--others', '--exclude-standard']).decode('utf-8').split('\0')
    files = sorted(set(p for p in tracked if p))
    for name in files:
        p = REPO/name
        if not p.is_file():
            raise FileNotFoundError(f'Indexed source missing from working tree: {name}')
        copy(p, source/PRODUCT/name)
    deps = source/'dependencies/windows'
    for p in (args.glfw_root/'include').rglob('*'):
        if p.is_file():copy(p, deps/'glfw/include'/p.relative_to(args.glfw_root/'include'))
    for name in ['glfw3dll.lib', 'glfw3.dll']:
        copy(args.glfw_root/'lib-vc2022'/name, deps/'glfw/lib-vc2022'/name)
    copy(args.glfw_root/'LICENSE.md', deps/'glfw/LICENSE.md')
    for p in (args.glew_root/'include').rglob('*'):
        if p.is_file():copy(p, deps/'glew/include'/p.relative_to(args.glew_root/'include'))
    for name in ['lib/Release/x64/glew32.lib', 'bin/Release/x64/glew32.dll', 'LICENSE.txt']:
        copy(args.glew_root/name, deps/'glew'/name)
    write(deps/'README.md', '''# Windows build SDK snapshots

GLFW 3.4: https://www.glfw.org/download.html
GLEW 2.1.0: https://glew.sourceforge.net/

These are the exact x64 headers / import libraries / DLLs used by this release.
Their license files are included. SDKs do not include MSVC, Windows SDK or CUDA Toolkit.
For upstream library source, use the official project links above.
''')
    write(source/'Build-Windows.cmd', f'@echo off\ncall "%~dp0{PRODUCT}\\tools\\build_windows_release.cmd" %*\nexit /b %errorlevel%\n')
    write(source/'README_重新构建.md', f'''# SeisWave Lab · Curvilinear v{version} 源码重建包

完整保留项目源码、CMake 配置、着色器、图标、示例、文档、Notebook、基准程序、第三方源码与许可。
dependencies/windows 包含本次使用的 GLFW 3.4 / GLEW 2.1.0 x64 SDK；无需原开发机 D 盘依赖目录。

## Windows 构建

1. 安装 Visual Studio 2022 或 Build Tools，选择“使用 C++ 的桌面开发”、Windows SDK 和 CMake / Ninja 工具。
2. 双击 Build-Windows.cmd，默认构建 CPU 版本；不需要 CUDA Toolkit。
3. CUDA 版本：先安装与 MSVC 兼容的 CUDA Toolkit 12.8+，在终端执行 `Build-Windows.cmd cuda`。
4. 主程序位于 `out/bin/Release/{PRODUCT}.exe`，源码文件夹内不会创建构建缓存。

可直接用 VS 打开 {PRODUCT}，按 docs/BUILDING.md 配置 SDK 路径；辅助构建脚本会自动使用相对 dependencies 路径。
CUDA 架构默认包含 sm_75 / sm_86 / sm_89 / sm_120 与 compute_75 PTX；较旧 Toolkit 需要缩减目标。

构建全部验证目标：`cmake --build out/build/x64-Release --parallel 4`。
执行数值测试：`ctest --test-dir out/build/x64-Release -C Release --output-on-failure`。
Python 绘图依赖与 DEM Notebook 依赖分别见项目 tools/requirements.txt、tools/dem_profiles/requirements.txt；运行 GUI 不需要 Python。

本包不包含 .git 历史、VS 个人缓存、编译中间产物、开发机 Python 环境和个人 DEM / 模拟结果。
BUILD_INFO.json 标识本次基准提交和工作区快照；FILE_MANIFEST.json 可核对每个发布文件。
''')
    source_fingerprint = hashlib.sha256('\n'.join(f'{name} {sha(REPO/name)}' for name in files).encode('utf-8')).hexdigest()
    info = {
        'product': PRODUCT, 'version': version,
        'version_source': 'VERSION', 'runtime_versions': runtime_versions,
        'created_at': datetime.now(timezone(timedelta(hours=8))).isoformat(),
        'base_git_commit': git('rev-parse', 'HEAD'),
        'git_working_tree_modified': bool(git('status', '--porcelain')),
        'source_tree_sha256': source_fingerprint, 'source_file_count': len(files),
        'configuration': 'Release', 'platform': 'Windows x64',
        'precision': '2D selectable Float32/Float64; acoustic default Float32; elastic and 3D default Float64',
        'cuda_toolkit': '12.8', 'cuda_architectures': '75-real;86-real;89-real;120-real;75-virtual',
        'main_exe_sha256': sha(binary/f'{PRODUCT}.exe'),
        'runtime_output': 'portable.flag => executable directory/userdata',
        'note': 'Exact source inventory and hashes are recorded; public releases must use the clean tagged commit.',
    }
    for folder in [portable, source]:
        write(folder/'BUILD_INFO.json', json.dumps(info, ensure_ascii=False, indent=2))
        manifests(folder)
    # Imported DLLs must resolve either next to the EXE or through Windows system DLLs.
    dlls = {p.name.lower() for p in portable.glob('*.dll')}
    system = {'opengl32.dll','comdlg32.dll','dxgi.dll','kernel32.dll','user32.dll','gdi32.dll','shell32.dll','imm32.dll','advapi32.dll','ntdll.dll','msvcrt.dll','ole32.dll','oleaut32.dll','winmm.dll','version.dll','ws2_32.dll','bcrypt.dll','shlwapi.dll','setupapi.dll','cfgmgr32.dll','powrprof.dll','secur32.dll','rpcrt4.dll','combase.dll','ucrtbase.dll','userenv.dll'}
    imports = {}; missing = set()
    for p in sorted([*portable.glob('*.exe'), *portable.glob('*.dll')]):
        dependencies = pe_imports(p);imports[p.name] = dependencies
        for dependency in dependencies:
            d = dependency.lower()
            if d not in dlls and d not in system and not d.startswith(('api-ms-win-', 'ext-ms-win-')):
                missing.add(d)
    write(root/'DEPENDENCY_AUDIT.json', json.dumps({'imports': imports, 'missing_non_system_dlls': sorted(missing)}, indent=2))
    if missing:raise RuntimeError(f'Missing runtime DLLs: {missing}')
    write(root/'STAGE.json', json.dumps({'version': version,'portable': portable.name,'source': source.name},indent=2))
    print(json.dumps({'staged': str(root),'portable': str(portable),'source': str(source),'source_files':len(files),'missing_dlls':[]},ensure_ascii=False))

def finalize(root: Path) -> None:
    info = json.loads((root/'STAGE.json').read_text(encoding='utf-8'))
    archives = []
    for key in ['portable','source']:
        folder = root/info[key]
        inventory = json.loads((folder/'FILE_MANIFEST.json').read_text(encoding='utf-8'))
        for entry in inventory:
            p=folder/entry['path']
            if sha(p)!=entry['sha256']:raise RuntimeError(f'Staged file changed: {p}')
        archive = root/(folder.name+'.zip')
        with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
            for entry in inventory:
                p=folder/entry['path'];z.write(p, folder.name+'/'+entry['path'])
            z.write(folder/'FILE_MANIFEST.json',folder.name+'/FILE_MANIFEST.json')
        with zipfile.ZipFile(archive,'r') as z:
            if z.testzip() is not None:raise RuntimeError('ZIP verification failed')
        archives.append(archive)
    write(root/'SHA256SUMS.txt',''.join(f'{sha(p)}  {p.name}\n' for p in archives))
    print(json.dumps([{'file':str(p),'bytes':p.stat().st_size,'sha256':sha(p)} for p in archives],ensure_ascii=False,indent=2))

def main() -> None:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--finalize',action='store_true')
    p.add_argument('--glfw-root',type=Path)
    p.add_argument('--glew-root',type=Path)
    p.add_argument('--vc-redist-root',type=Path)
    p.add_argument('--cuda-root',type=Path)
    args=p.parse_args();root=args.output.resolve()
    if not root.is_relative_to(OUT.resolve()) or root==OUT.resolve():
        p.error('Release output must be a new subdirectory inside this workspace out.')
    version=project_version()
    if args.finalize:finalize(root)
    else:
        if not all([args.glfw_root,args.glew_root,args.vc_redist_root,args.cuda_root]):p.error('Stage requires all four SDK/runtime roots.')
        stage(args,root,version)

if __name__=='__main__':main()
