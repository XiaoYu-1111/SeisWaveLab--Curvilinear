"""Publish a verified benchmark snapshot into repository documentation on request."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = args.results.resolve()
    assert source.is_relative_to(repo.parent / 'out'), 'Expected results in project outer out'
    assert (source / 'COMPLETE.txt').is_file(), 'Incomplete benchmark'
    data = repo / 'docs/data/performance-2026-10-19'
    images = repo / 'docs/images/performance'
    data.mkdir(parents=True, exist_ok=True)
    images.mkdir(parents=True, exist_ok=True)
    copied = []
    for name in ['trials.csv', 'summary.csv', 'hardware.json', 'COMPLETE.txt']:
        shutil.copy2(source / name, data / name)
        assert (source / name).read_bytes() == (data / name).read_bytes()
        copied.append(data / name)
    for name in ['gpu-start.csv', 'gpu-end.csv', 'power-plan.txt']:
        shutil.copy2(source.parent / name, data / name)
        copied.append(data / name)
    provenance = json.loads((source.parent / 'provenance.json').read_text(encoding='utf-8-sig'))
    # Use repository-relative paths while preserving measured-source hashes.
    for entry in provenance['solverHashes']:
        entry['Path'] = Path(entry['Path']).relative_to(repo).as_posix()
    (data / 'provenance.json').write_text(json.dumps(provenance, ensure_ascii=False, indent=2), encoding='utf-8')
    copied.append(data / 'provenance.json')
    for name in ['performance.png', 'speedup.png', 'speedup.svg']:
        shutil.copy2(source / name, images / name)
        assert (source / name).read_bytes() == (images / name).read_bytes()
        copied.append(images / name)
    report = (source / 'REPORT.md').read_text(encoding='utf-8')
    for name in ['trials.csv', 'summary.csv', 'hardware.json', 'COMPLETE.txt']:
        report = report.replace(f']({name})', f'](data/performance-2026-10-19/{name})')
    for name in ['speedup.png', 'speedup.svg']:
        report = report.replace(f']({name})', f'](images/performance/{name})')
    report = report.replace('## 性能结果\n', '## 性能结果\n\n![CPU 与 CUDA 单步耗时对比](images/performance/performance.png)\n')
    report = report.replace('- 上一级目录包含 `provenance.json`（源码 SHA-256/提交信息）、`formal.log`、`gpu-start.csv` 与电源计划。',
                            '- [源码版本与 SHA-256](data/performance-2026-10-19/provenance.json)、[GPU 测试前状态](data/performance-2026-10-19/gpu-start.csv)、[GPU 测试后状态](data/performance-2026-10-19/gpu-end.csv)、[电源计划](data/performance-2026-10-19/power-plan.txt)。逐轮实测 CSV 是正式记录，原始运行日志留在外层 out。')
    destination = repo / 'docs/性能基准.md'
    destination.write_text(report, encoding='utf-8')
    copied.append(destination)
    manifest = {p.relative_to(repo).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in copied}
    (data / 'publication-sha256.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'Published {len(copied)} files and SHA-256 manifest: {destination}')


if __name__ == '__main__':
    main()
