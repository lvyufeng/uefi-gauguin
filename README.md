# uefi-gauguin

Mu-Silicium UEFI port for the Xiaomi Redmi Note 9 Pro 5G / Mi 10T Lite
(`gauguin`, Qualcomm **SM7125 "lagoon"**), the firmware half of the
[UniXNU-gauguin](https://github.com/lvyufeng/UniXNU-gauguin) device port.

The UEFI environment supplies what UniXNU and DarwinBoot need
([layer model](https://unixnu.com/guide/overview)):

| Service | Consumer |
| --- | --- |
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` | UniXNU menu, framebuffer handoff, XNU console |
| `EFI_SIMPLE_TEXT_IN_PROTOCOL` | menu input |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` | configuration and payload |
| `EFI_BLOCK_IO_PROTOCOL` | `userdata` and block slices |
| `EFI_SERIAL_IO_PROTOCOL` | logs |

## Provenance

This port derives from the `miatoll` Mu-Silicium package: `miatoll` is Xiaomi's
codename for the SM7125 family, so the SoC support, memory map and firmware
binaries carry over directly. What changes for gauguin is the device
configuration, the UEFI device tree and the firmware-volume addresses.

Upstream: [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium),
reference port layout [UniXNU/uefi-sheng](https://github.com/UniXNU/uefi-sheng).

## State

Early port. See [UniXNU-gauguin](https://github.com/lvyufeng/UniXNU-gauguin) for
the device repository and the bring-up order
([docs](https://unixnu.com/porting/bring-up)).
