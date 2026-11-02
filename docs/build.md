# Build and source layout

The upstream base is **official QEMU 10.2.4**, commit `3e0bcba1ca7d6607ca49a988d165f052a3a53323`. The annotated tag object is different from the commit; the integrator checks the commit.

`qemu/source-manifest.json` enumerates the maintained source files and the original upstream hashes for overwritten files. `qemu/prepare_source.py` validates all destinations before writing any file. Repeating it is safe: files may match the official baseline, the previous installed version, or the desired version. Unrecognized edits are rejected so another worktree is not silently overwritten.

```sh
# Automatic official source checkout in .cache/source
python3 tools/build.py --jobs 4

# Existing official source, without downloading that source
python3 tools/build.py --source /path/to/qemu --build-dir /path/to/build --jobs 4
```

The build targets `aarch64-softmmu`, enables plugins, and builds `qemu-system-aarch64` plus `qemu-img`. Normal launches load no observational plugins. QEMU's configure step may obtain its own build dependencies; none of the build tools fetch Nikon firmware.

The supported execution host is Linux, including WSL. Python requires Pillow for sensor conversion. A normal desktop Linux development environment additionally needs GCC/Clang, a C++ compiler, Git, Ninja, pkg-config, Python/venv, GLib and pixman development files, libjpeg development files, and x264/x265 development files. Consult QEMU's configure diagnostics for platform-specific dependencies.

## Source of truth

- `qemu/hw/`, `qemu/include/`, `qemu/target/`, `qemu/configs/`: editable C, header and build sources, including the shared EXPEED models.
- `zf_qemu/`: portable configuration, firmware hash checks, shared launch profile, FAT32 media tools and WebUI.
- `tests/`: synthetic contracts; no firmware, captured pixels or dumps.
- `.cache/`: ignored upstream source and build output, recreated per worktree.

The installed QEMU source is build material. Edit this repository's maintained files and rerun the build. Use normal Git branches/worktrees from this repository's `main` for further work.

## Publication

Run the public tests and scanner, inspect `git diff --cached`, and archive the reviewed commit with `git archive`. Scan the extracted archive again before uploading. Do not commit local configuration, state directories, runtime manifests, photos, dumps, firmware, keys or research records. The repository's input fingerprint list contains only names, lengths and SHA256 values.
