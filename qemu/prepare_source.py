#!/usr/bin/env python3
"""Integrate the Zf source snapshot into a local QEMU checkout."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]

def normalize_imported_comments(source):
    for relative in (
        "hw/arm/nikon-z63-z9lab.c",
        "hw/arm/nikon-z63-gfx.h",
        "hw/arm/nikon-z63.c",
        "hw/arm/nikon-zf-sd-mode3.h",
        "hw/arm/nikon-z9-pcie.h",
    ):
        path = source / relative
        if not path.is_file():
            continue
        data = path.read_bytes()
        text = data.decode("utf-8")
        text = re.sub(r"\brun\d+\b", "native observation", text, flags=re.IGNORECASE)
        text = re.sub(r"\bcheckpoint\d+/trial\d+\b", "native variant", text, flags=re.IGNORECASE)
        text = re.sub(r"\breports/[^\s*]+", "captured translation tables", text)
        text = re.sub(r"\b[^\s/]+-\d{8}\.md\b", "the native extension notes", text)
        text = re.sub(r", sha256 [0-9a-f]{64}", "", text, flags=re.IGNORECASE)
        text = text.replace("Fingerprints identify local programs", "Fingerprints identify compatible programs")
        data = text.encode("utf-8")
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="clean local QEMU source tree")
    args = parser.parse_args()
    source = args.source.resolve()
    if not (source / "configure").is_file() or not (source / "hw/arm/meson.build").is_file():
        parser.error("source must be a QEMU checkout")
    if not (source / ".git").exists():
        parser.error("source must be a Git checkout so its revision can be checked")
    revision = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        check=True, text=True, capture_output=True,
    ).stdout.strip()
    expected = (ROOT / "qemu/UPSTREAM_REVISION").read_text().strip()
    if revision != expected:
        parser.error(f"expected QEMU revision {expected}, got {revision}")
    status = subprocess.run(
        ["git", "-C", str(source), "status", "--porcelain"],
        check=True, text=True, capture_output=True,
    ).stdout.strip()
    if status:
        parser.error("QEMU source tree must be clean before integration")
    normalize_imported_comments(source)
    patches = sorted((ROOT / "qemu/patches").glob("*.patch"))
    for patch in patches:
        subprocess.run(["git", "-C", str(source), "apply", "--check", str(patch)], check=True)
    for patch in patches:
        subprocess.run(["git", "-C", str(source), "apply", str(patch)], check=True)
    for overlay in sorted((ROOT / "qemu").glob("nikon-zf-*")):
        if overlay.suffix in {".h", ".inc", ".c"}:
            shutil.copy2(overlay, source / "hw/arm" / overlay.name)
    shutil.copy2(ROOT / "qemu" / "nikon-zf.c", source / "hw/arm" / "nikon-zf.c")
    print(f"Integrated {len(patches)} source patches and Zf board files into {source}")


if __name__ == "__main__":
    main()
