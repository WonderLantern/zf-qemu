# Build Notes

The source snapshot was assembled from the local Zf integration against QEMU revision `055b82d29350367f2036f70793a5aa332cf08936`. It modifies shared EXPEED 7/QEMU device code and adds the `nikon-zf` machine. `tools/prepare_source.py` validates the revision and clean Git state, checks all source patches, applies them, and installs the Zf board source files into `hw/arm/`.

The contract suite is intentionally independent of QEMU source, Nikon firmware, and private run data. It validates selected clock, input, matrix, PCIe, RAM-diagnostic, and RNG model boundaries. A successful contract run is not a full QEMU build or guest boot.

For a full integration build, provide a separate clean QEMU checkout and an external build directory to `tools/build.py`. The script does not fetch source or firmware. Do not point it at an active research worktree. QEMU build dependencies vary by platform; use the documentation for the exact QEMU revision.

This public source tree does not include the local firmware runner or provisioning scripts because those scripts embed workstation paths and bind exact private firmware identities. A future portable launcher should accept local paths as explicit arguments and must not create outputs inside the source tree by default.
