#!/usr/bin/env python3
"""Build the Zf machine in a local QEMU source checkout without downloading."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="clean local QEMU checkout")
    parser.add_argument("--build-dir", type=Path, help="external build directory (default: <source>/build-zf)")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error("--jobs must be between 1 and 64")

    source = args.source.resolve()
    subprocess.run(["python3", str(ROOT / "qemu/prepare_source.py"), str(source)], check=True)
    build = (args.build_dir or source / "build-zf").resolve()
    build.mkdir(parents=True, exist_ok=True)
    if not (build / "build.ninja").is_file():
        subprocess.run([
            str(source / "configure"), "--target-list=aarch64-softmmu",
            "--enable-plugins", "--disable-werror",
        ], cwd=build, check=True)
    subprocess.run(["ninja", "-C", str(build), "-j", str(args.jobs), "qemu-system-aarch64"], check=True)
    subprocess.run([str(build / "qemu-system-aarch64"), "--version"], check=True)


if __name__ == "__main__":
    main()
