# Verification status

This is an experimental functional model, not a hardware specification or a faithful physical-camera replacement. Private firmware and runtime evidence are intentionally excluded from the public repository.

| Area | Implemented behavior and limits |
| --- | --- |
| Build | Maintained EXPEED/Zf sources integrate over official QEMU 10.2.4; builds both the AArch64 system emulator and qemu-img. No local research patch generators are needed. |
| Launch | CLI and WebUI share configuration, fixed firmware hashing, readability checks for local SNOR/NAND images, explicit EG/LI/EX loading and the same functional profile. No default duration limit. A default boot without an initialized NAND template reached Linux kernel startup, LaMainApp startup and the BusyBox shell. This does not qualify every Linux service; QMP running alone is insufficient evidence. |
| Displays | Separate native LCD/EVF surfaces and 3 FPS host snapshots, with original OSD retained. Test pictures enter the Bayer pipeline. EVF stride/scanline artifacts and some display-mode behavior remain incomplete. |
| Top display | Native segment protocol is exposed as an 8+7 segment display with one decimal point. No input renders no content. Full countdown/exposure fidelity is not qualified. |
| Inputs | Matrix/menu/navigation, named buttons, shutter stages, Fn hold/lock, mode/eye selectors, encoders and retro contacts are modeled. All engraved dial-to-contact mappings are not established; unsupported ISO 512000 remains explicitly unbound. Effective exposure also depends on firmware and lens policy. |
| Storage | Two virtual backends, command/data/DMA/IRQ paths, FAT32 file uploads and per-run writable overlays. UHS-II physical behavior and complete dual-card photo policy are unqualified. |
| Still capture | A bounded synthetic-source JPEG path exists. Repeat capture, recovery, RAW acceptance and all quality/card combinations remain incomplete. |
| Video | Recording is not qualified or advertised as functional. |
| Sensor/lens | Fixed 6048 × 4032 Bayer input, generated test chart and user images; functional lens/battery stand-ins. No optical, analog or physical authentication claims. |

The public C and Python tests use synthetic data and test selected contracts. Passing them is distinct from guest boot, photograph quality or camera behavior. Consult runtime logs locally when investigating unsupported behavior; do not publish those logs or input files.
