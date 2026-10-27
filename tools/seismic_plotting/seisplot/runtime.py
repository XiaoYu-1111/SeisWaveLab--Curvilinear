"""Make the active Conda DLL directory visible without changing the user's environment."""
import os,sys
from pathlib import Path
_handles=[]
def prepare_runtime():
    # Respect an explicitly selected cache; otherwise keep generated caches under outer out.
    os.environ.setdefault('MPLCONFIGDIR',str(Path(__file__).resolve().parents[4]/'out/cache/seismic_plotting/matplotlib'))
    library=Path(sys.prefix)/'Library/bin'
    if sys.platform=='win32' and library.is_dir() and hasattr(os,'add_dll_directory'):
        _handles.append(os.add_dll_directory(str(library)))
        if str(library) not in os.environ.get('PATH','').split(os.pathsep):os.environ['PATH']=str(library)+os.pathsep+os.environ.get('PATH','')
