"""Shared release/documentation version, sourced exclusively from ../VERSION."""
from pathlib import Path
import re

def project_version() -> str:
    value=(Path(__file__).resolve().parents[1]/'VERSION').read_text(encoding='utf-8').strip()
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)',value):
        raise ValueError('VERSION must contain a single major.minor.patch version')
    return value

if __name__=='__main__':
    print(project_version())
