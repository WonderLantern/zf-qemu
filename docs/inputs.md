# Local input requirements

The launcher checks the following fixed Zf C3.01 firmware components by length and SHA256 before a VM is launched, including when Python runs with optimization enabled. Filenames do not matter; paths come from your local JSON configuration.

| Configuration key | Bytes | SHA256 |
| --- | ---: | --- |
| `eg` | 79,144,343 | `0b53631396314e17e5b5a46a6758a35ae521c1e8bddc2fb59b61bcf72e4889f8` |
| `loader` | 1,441,792 | `e32200ad98236a857e69b72a62c097b3e65894f4a36801a66c843b038b13decc` |
| `li` | 27,254,786 | `71e098bf2decb893505fec55d97b042c0b064a31a42a270fed645ca5dd0b112e` |
| `mcu` (EX) | 524,288 | `3211aabab930bb63baca1faecb6a4e9ff5d569f306f064c6a9c0e4865044bb74` |

No input files or firmware extraction/decryption tools are supplied. A matching fingerprint identifies the supported byte sequence; it is not a manufacturer authentication, signing or flashing result.

The shared CLI/WebUI profile requires these four firmware components plus a local `snor_user` image. LI is passed explicitly as `li-image`; presence of a NAND file never substitutes for that argument.

Local image parameters:

| Configuration key | What to provide |
| --- | --- |
| `snor_user` (required) | A raw SNOR_USER serial-flash user-region image exported from your own Zf. Supply the user-region image, rather than EG, LI, a complete firmware update package or an unprocessed whole-flash dump. The region may contain device-specific settings and logs; keep it private. |
| `nand` (optional) | An existing emulated NAND image compatible with this model, such as a locally provisioned template containing LI. Supply the NAND image itself, rather than the LI component file. Omit this key if you have no template; do not use an empty string as its value. |

For example, these entries belong inside your local configuration's `inputs` object, alongside the four fixed firmware paths:

```json
{
  "snor_user": "/path/to/private/snor-user.bin",
  "nand": "/path/to/private/nand-template.img"
}
```

Both images can vary between users and runs. The launcher checks only that these files exist and can be read; it neither computes their SHA256 nor compares their lengths against fixed values. No individual image sizes or fingerprints are published. These files must still be compatible with the model's image format and geometry; a readable file alone does not guarantee a successful guest boot.

The optional NAND template is copied into each fresh run before use. Without it, the model uses its default NAND initialization; this is not a claim of an equivalent provisioned filesystem.

The optional configuration `media` object accepts `sd` and `microsd` paths to local raw FAT32 templates. They are copied into private state storage before use. Otherwise the launcher generates empty synthetic cards. Guest writes use per-run qcow2 overlays. These cards are user data, not authenticated firmware inputs, and have no fixed expected hash.

Generated sensor input always contains 6048 × 4032 RGGB samples at 14 bits in little-endian 16-bit storage. Pictures are EXIF-oriented, resized to fit without stretching/cropping, and padded with black. This models an active image raster; it does not model optical-black margins, noise, optics or a physical sensor's full behavior.
