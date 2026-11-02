#!/usr/bin/env python3
"""Scan public source trees or extracted release archives for private artifacts."""
from pathlib import Path
import hashlib
import re
import sys

FORBIDDEN = {
    '.bin', '.img', '.raw', '.qcow2', '.dump', '.rom', '.elf', '.so', '.exe',
    '.ppm', '.png', '.jpg', '.jpeg', '.nef', '.mov', '.mp4', '.log', '.jsonl',
    '.candidate', '.payload', '.nz9raw', '.zip', '.7z', '.gz', '.xz', '.tar',
}
TOP_LEVEL = {
    '.gitignore', 'AGENTS.md', 'COPYING', 'LICENSES', 'NOTICE.md', 'README.md',
    'docs', 'publication-manifest.json', 'qemu', 'tests', 'tools', 'zf_qemu',
    'config.example.json', 'requirements.txt',
}
PRIVATE = (
    re.compile(r'(?i)\b[a-z]:[/\\](?:users|nikon-firmware|codexscratch)[/\\]'),
    re.compile(r'(?i)/home/[^/\s]+/|/mnt/[a-z]/(?:users|nikon-firmware)/'),
    re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'),
    re.compile(r'(?i)(?:private[_ -]?key|secret[_ -]?key)\s*[:=]\s*[\x22\x27]?[A-Za-z0-9+/]{24,}'),
    re.compile(r'(?i)\b(?:gh[pousr]_[a-z0-9]{25,}|github_pat_[a-z0-9_]{25,})\b'),
    re.compile(r'(?i)(?:reports|notes)/[^\s\x22\x27]+'),
    re.compile(r'(?i)\b[a-z0-9_-]+-20\d{6,8}\.md\b'),
    re.compile(r'(?i)\b(?:boot|run|checkpoint|trial)\d+\b'),
)

# Reviewed documentation screenshot only; all other binary/media files are rejected.
PUBLIC_IMAGES = {
    'docs/images/webui.png': '0ef62b3417dcd37f9da23b2e5b2e45126681a69f6fc651f5693dd5babf2d599a',
}


def scan(root):
    errors = []
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root)
        if rel.parts[0] in {'.git', '.cache', '.venv'} or '__pycache__' in rel.parts:
            continue
        if path.is_symlink():
            errors.append(f'symlink: {rel}')
            continue
        if not path.is_file():
            continue
        if rel.parts[0] not in TOP_LEVEL:
            errors.append(f'unexpected release path: {rel}')
        if rel.as_posix() in PUBLIC_IMAGES:
            if hashlib.sha256(path.read_bytes()).hexdigest() != PUBLIC_IMAGES[rel.as_posix()]:
                errors.append(f'unreviewed documentation image: {rel}')
            continue
        if path.suffix.lower() in FORBIDDEN:
            errors.append(f'forbidden artifact: {rel}')
            continue
        if path.stat().st_size > 8 * 1024**2:
            errors.append(f'unexpected large source file: {rel}')
            continue
        try:
            text = path.read_text(encoding='utf-8')
        except (UnicodeError, OSError):
            errors.append(f'non-text file: {rel}')
            continue
        if '\0' in text:
            errors.append(f'binary content: {rel}')
        if rel.as_posix() in {'tests/check_release_tree.py', 'tests/test_release_tree.py'}:
            continue
        for index, line in enumerate(text.splitlines(), 1):
            if any(pattern.search(line) for pattern in PRIVATE):
                errors.append(f'possible private material: {rel}:{index}')
    return errors


def main():
    errors = scan(Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve())
    if errors:
        print('FAIL: release privacy scan\n' + '\n'.join(errors))
        raise SystemExit(1)
    print('PASS: source-only release privacy scan')


if __name__ == '__main__':
    main()
