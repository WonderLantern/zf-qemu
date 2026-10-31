#!/usr/bin/env python3
"""Compile and run firmware-free C contracts for selected Zf models."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "clock-contract.c",
    "gpio22-input-contract.c",
    "matrix-contract.c",
    "pcie-contract.c",
    "ramdump-contract.c",
    "rng-contract.c",
)


def main():
    tests = ROOT / "tests"
    with tempfile.TemporaryDirectory(prefix="zf-qemu-contracts-") as temp:
        out = Path(temp)
        for source in SOURCES:
            name = Path(source).stem
            command = [
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                "-Wno-unused-parameter", "-I", str(tests / "include"),
                str(tests / source), "-o", str(out / name),
            ]
            subprocess.run(command, check=True)
            subprocess.run([str(out / name)], check=True)
    print(f"PASS: {len(SOURCES)} firmware-free C contracts")


if __name__ == "__main__":
    main()
