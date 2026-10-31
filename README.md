# Nikon Zf QEMU

An experimental QEMU machine model for studying selected Nikon Zf C3.01 EXPEED 7 behavior. The model reuses shared EXPEED 7 devices and adds Zf-specific board, display, input, storage, and capture integration.

This is a source-only project. It does not include Nikon firmware, decrypted payloads, EX/EG/LI images, flash or NAND dumps, cryptographic keys, calibration data, private run logs, or screenshots. The build does not download firmware or access a physical camera. This project is independent and is not affiliated with or endorsed by Nikon Corporation.

The model is incomplete and is not a camera emulator or hardware specification. See [Verification Status](docs/verification-status.md) for current tested areas and limitations.

## Contents

- `qemu/` contains the Zf board sources, integration patch, and pinned upstream QEMU revision.
- `tools/` contains the source integration and build entry points.
- `tests/` contains firmware-free C contract tests and the release-tree privacy scanner.
- `docs/` contains build details and verification limits.
- `COPYING`, `LICENSES/`, and `NOTICE.md` contain licensing and attribution information.

Passive observation plugins, experiment scripts, run manifests, research notes, firmware, and generated artifacts are not part of this source tree.

## Requirements

For the contract tests, install Python 3 and a C compiler such as GCC or Clang. A full QEMU build additionally needs the dependencies listed by the upstream QEMU documentation for your platform.

The release is pinned to the QEMU revision in [`qemu/UPSTREAM_REVISION`](qemu/UPSTREAM_REVISION):

`055b82d29350367f2036f70793a5aa332cf08936`

## Build

Use a clean checkout of that QEMU revision and an external build directory:

```sh
python3 tools/build.py \
    --source /path/to/qemu \
    --build-dir /path/to/qemu-build-zf \
    --jobs 4
```

The integrator checks the revision and clean Git state, applies the reviewed patch, and copies the Zf board files into `hw/arm/`. It does not fetch QEMU or firmware. The build targets `aarch64-softmmu` and produces `qemu-system-aarch64`.

## Tests

Run the firmware-free contract suite and release-tree scanner from the repository root:

```sh
python3 tests/run_contracts.py
python3 tests/check_release_tree.py .
```

The contract suite checks selected clock, GPIO, matrix, PCIe, diagnostic-read, and RNG boundaries. The privacy scanner rejects common firmware, dump, media, private-path, key-material, and research-artifact leaks. These checks do not boot Nikon firmware or qualify physical-camera behavior.

## Use

The machine type is `nikon-zf`. Compatible firmware or media must be supplied locally through QEMU's normal machine properties and block backends. Keep those inputs, run directories, and generated photos outside this source tree. This repository does not provide firmware acquisition, decryption, provisioning, or camera-control tools.

For the build and test boundary, see [docs/build.md](docs/build.md). For the current modeled areas and known limitations, see [docs/verification-status.md](docs/verification-status.md).

## Acknowledgements

This work draws on shared EXPEED 7 device models and research methods from [WonderLantern/z9-qemu](https://github.com/WonderLantern/z9-qemu), [WonderLantern/z63-qemu](https://github.com/WonderLantern/z63-qemu), and [WonderLantern/zr-qemu](https://github.com/WonderLantern/zr-qemu). Thank you to their authors and contributors. Their code retains its original copyright, attribution, and license terms; see [NOTICE.md](NOTICE.md).

## License

Zf-authored model sources are distributed under the GNU General Public License, version 2 or, at your option, any later version, matching the license used by `z9-qemu`. QEMU, shared EXPEED 7 code, and other third-party components retain their applicable upstream licenses. Preserve all notices when redistributing; see [`COPYING`](COPYING), [`LICENSES/QEMU-LICENSE.txt`](LICENSES/QEMU-LICENSE.txt), and [NOTICE.md](NOTICE.md).
