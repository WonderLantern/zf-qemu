# Verification Status

This status summarizes the latest available local evidence checked on 2026-10-31. The evidence files and private runtime outputs are not included here. Status is scoped to the modeled behavior and test inputs; it is not a statement of physical-camera fidelity.

| Area | Current evidence | Release claim |
| --- | --- | --- |
| EVF and rear display | Original display mode and eye-sensor transitions were observed. A host-side EVF preview passed pixel checks with synthetic static fixtures. The original direct EVF frame request is not restored; the EVF image center remained black in native runs. | Partial display routing and diagnostic preview only; live EVF not qualified. |
| Folding screen | LCD/EVF host surfaces can switch independently. Physical hinge angle/orientation is not bound to an input or state model. | Not modeled as a fold-angle workflow. |
| Top aperture display | Segment protocol/core tests cover digit patterns and recorded native writes. Full original exposure/countdown behavior is not verified. | Segment renderer and protocol subset only. |
| Buttons and dials | Selected matrix contacts, shutter stages, navigation, menu, named buttons, lever and encoder contacts have contract or bounded native-input checks. Complete UI semantics and all physical positions are not proven. | Partial input model. |
| Two storage media | Two virtual backends, card command/data paths, DMA/IRQ isolation, simultaneous card reads/writes, and original-firmware dual-media mounting/menu display have been tested. UHS-II physical behavior and camera photo routing across both cards have not. | Dual virtual media paths; dual-card capture policy unverified. |
| Still capture | One synthetic-source JPEG was written by the original application and independently decoded. Later runs reproduced the file structure. Post-shot UI recovery has shown an error; repeat capture, RAW+JPEG, RAW acceptance, and dual-card routing are not qualified. | A bounded single JPEG path only. |
| Video | The latest native recording-service observation did not reach the original video service/API callback points. The encoder model has no accepted pixel output/completion evidence; an earlier reset path reached a guest panic on unmapped accesses. | Video recording is not functional or qualified. |

## Release Test Boundary

The public C contracts are repeatable without private inputs. A clean full QEMU build and end-to-end firmware run must be performed by the publisher in an isolated environment with locally supplied inputs before claiming those levels of validation. Do not promote modeled or synthetic behavior to physical-device behavior.
