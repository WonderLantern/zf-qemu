#!/usr/bin/env python3
"""Reject common private inputs, local paths, and key material in release trees."""
from pathlib import Path
import re
import sys


FORBIDDEN_SUFFIXES = {
    ".bin", ".img", ".raw", ".qcow2", ".dump", ".rom", ".elf", ".so",
    ".ppm", ".png", ".jpg", ".jpeg", ".nef", ".mov", ".mp4", ".log",
}
PATH_PATTERNS = (
    re.compile(r"(?i)[a-z]:[/\\](?:users|nikon-firmware|codexscratch)[/\\]"),
    re.compile(r"(?i)(?:/home/[^/\s]+/|/mnt/[a-z]/nikon-firmware/)"),
    re.compile(r"(?i)EX-flash-[^\s'\"]+\.bin|nand\.img|SNOR_USER[^\s'\"]*\.bin"),
)
KEY_PATTERNS = (
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----"),
    re.compile(r"(?i)(?:private[_ -]?key|secret[_ -]?key)\s*[:=]\s*['\"]?[A-Za-z0-9+/]{24,}"),
)
RESEARCH_MARKER_PATTERNS = (
    re.compile(r"(?i)\b(?:run|checkpoint|trial)\d+\b"),
    re.compile(r"(?i)(?:reports|notes)/[^\s'\"]+"),
    re.compile(r"(?i)\b[a-z0-9_-]+-20\d{6,8}\.md\b"),
)


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    errors = []
    for path in sorted(root.rglob("*")):
        if "__pycache__" in path.parts:
            errors.append(f"generated Python cache must not be published: {path.relative_to(root)}")
            continue
        if not path.is_file():
            continue
        rel = path.relative_to(root)
        if rel.parts[0] == "plugins":
            errors.append(f"research plugin source must not be published: {rel}")
            continue
        if rel.as_posix() == "tests/check_release_tree.py":
            continue
        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            errors.append(f"forbidden artifact extension: {rel}")
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            errors.append(f"non-text file: {rel}")
            continue
        for line_number, line in enumerate(text.splitlines(), 1):
            if any(pattern.search(line) for pattern in PATH_PATTERNS):
                errors.append(f"private path: {rel}:{line_number}")
            if any(pattern.search(line) for pattern in KEY_PATTERNS):
                errors.append(f"key-like material: {rel}:{line_number}")
            if rel.as_posix() != "qemu/prepare_source.py" and any(
                pattern.search(line) for pattern in RESEARCH_MARKER_PATTERNS
            ):
                errors.append(f"specific research batch marker: {rel}:{line_number}")
    if errors:
        print("FAIL: release-tree scan found possible private material")
        print("\n".join(errors))
        raise SystemExit(1)
    print("PASS: no forbidden artifacts, common private paths, or key markers found")


if __name__ == "__main__":
    main()
