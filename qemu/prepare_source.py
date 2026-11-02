#!/usr/bin/env python3
"""Install maintained model sources over the pinned official QEMU checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def prepare(source):
    source = Path(source).resolve()
    manifest = json.loads((ROOT / 'source-manifest.json').read_text())
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != manifest['revision']:
        raise ValueError(f'Expected official QEMU {manifest["version"]} ({manifest["revision"]}), got {revision}')
    marker = Path(subprocess.check_output(['git', '-C', str(source), 'rev-parse', '--git-path',
                                          'zf-installed.json'], text=True).strip())
    if not marker.is_absolute():
        marker = source / marker
    previous = json.loads(marker.read_text()) if marker.exists() else {}
    intended = {}
    for relative, record in manifest['files'].items():
        destination = source / relative
        current = sha(destination)
        desired = sha(ROOT / relative)
        if desired is None:
            raise ValueError(f'Missing model source: {relative}')
        if current not in (record['upstream_sha256'], previous.get(relative), desired):
            raise ValueError(f'Preserving local QEMU edits in {relative}; edit the maintained qemu/ source or use a clean checkout')
        intended[relative] = desired
    for relative in intended:
        destination = source / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if sha(destination) != intended[relative]:
            destination.write_bytes((ROOT / relative).read_bytes())
    marker.write_text(json.dumps(intended, indent=2) + '\n')
    print(f'Integrated {len(intended)} maintained source files (no patch generators).')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    try:
        prepare(args.source)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
