#!/usr/bin/env python3
"""Check firmware operational docs against public ABI, hardware and CPU targets."""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(root=ROOT):
    root = Path(root)
    header = (root / 'contract/include/aster/system.h').read_text()
    abi = '.'.join(re.search(r'#define\s+ASTER_ABI_' + key + r'\s+(\d+)', header)[1]
                   for key in ('MAJOR', 'MINOR', 'PATCH'))
    hardware = [json.loads(p.read_text())['public'] for p in (root / 'hardware').glob('*.json')]
    ids = {h['id'] for h in hardware}
    cpus = {h['cpu'] for h in hardware}
    errors = []
    architecture = (root / 'docs/architecture.md').read_text()
    if not re.search(r'当前 ABI 为 ' + re.escape(abi) + r'。', architecture):
        errors.append(f'docs/architecture.md: current ABI must match public header ({abi})')
    block = re.search(r'<!-- hardware-models:start -->(.*?)<!-- hardware-models:end -->', architecture, re.S)
    documented = set(re.findall(r'^- `([^`]+)`', block[1], re.M)) if block else set()
    if documented != ids:
        errors.append(f'docs/architecture.md: hardware list {sorted(documented)} differs from hardware/ {sorted(ids)}')
    paths = [root / 'README.md']
    for folder in ('docs', 'boards'):
        paths.extend((root / folder).rglob('*.md'))
    for path in sorted(set(paths)):
        if not path.is_file():
            continue
        for number, line in enumerate(path.read_text().splitlines(), 1):
            where = f'{path.relative_to(root)}:{number}'
            if '当前只有一个硬件型号' in line and len(ids) != 1:
                errors.append(f'{where}: obsolete single-hardware claim')
            # Firmware build directories are board-specific; only SDK commands use CPU dirs.
            if not re.search(r'\baster\s+(?:build|deploy|verify|init|probe|push|run)\b', line):
                continue
            for value in re.findall(r'--target(?:\s+|=)[\x60\"\']?([A-Za-z0-9_-]+)', line):
                if not value.isupper() and value not in cpus:
                    errors.append(f'{where}: SDK --target {value} is not a CPU target ({sorted(cpus)})')
            for value in re.findall(r'(?:dist|build)[/\\]([a-z0-9-]+)[/\\]', line):
                if value in ids:
                    errors.append(f'{where}: SDK output directory must use CPU target, not {value}')
    if errors:
        raise ValueError('\n'.join(errors))
    return f'Firmware docs checked: ABI {abi}; hardware {", ".join(sorted(ids))}; CPU {", ".join(sorted(cpus))}'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        print(check(args.root))
    except ValueError as error:
        parser.exit(1, f'{error}\n')
