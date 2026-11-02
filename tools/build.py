#!/usr/bin/env python3
"""Build Zf QEMU from maintained sources and official QEMU 10.2.4."""
import argparse
from pathlib import Path
import subprocess
import sys
import json

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="official QEMU checkout; defaults to an automatic source-only clone in .cache")
    parser.add_argument("--build-dir", type=Path, help="build directory (default: .cache/build)")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error("--jobs must be between 1 and 64")

    source = (args.source or ROOT / '.cache/source').resolve()
    if not args.source and not source.exists():
        manifest = json.loads((ROOT / 'qemu/source-manifest.json').read_text())
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--depth', '1', '--branch', 'v' + manifest['version'],
                        manifest['upstream'], str(source)], check=True)
    subprocess.run([sys.executable, str(ROOT / "qemu/prepare_source.py"), str(source)], check=True)
    build = (args.build_dir or ROOT / '.cache/build').resolve()
    build.mkdir(parents=True, exist_ok=True)
    if not (build / "build.ninja").is_file():
        subprocess.run([
            str(source / "configure"), "--target-list=aarch64-softmmu",
            "--enable-plugins", "--disable-werror", "--disable-pvg", "--disable-docs",
        ], cwd=build, check=True)
    subprocess.run(["ninja", "-C", str(build), "-j", str(args.jobs), "qemu-system-aarch64", "qemu-img"], check=True)
    subprocess.run([str(build / "qemu-system-aarch64"), "--version"], check=True)


if __name__ == "__main__":
    main()
