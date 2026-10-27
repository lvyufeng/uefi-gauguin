# uefi-gauguin

Mu-Silicium UEFI port for the Xiaomi Redmi Note 9 Pro 5G / Mi 10T Lite
(`gauguin`, Qualcomm **SM7225** Snapdragon 750G, Adreno **619**), the firmware
half of the [UniXNU-gauguin](https://github.com/lvyufeng/UniXNU-gauguin)
device port.

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

The layout follows the `miatoll` Mu-Silicium package, which is the closest
existing Qualcomm device port and a reasonable structural starting point. It is
not this phone's SoC support: `gauguin` is the SM7225, and every platform
constant here - the memory map, the firmware-volume base and size, the ADT and
the panel - has been derived from this phone's own `/proc/iomem`, its device
tree and the signed drivers it ships, not from miatoll's. Check any inherited
value against the device before trusting it.

Upstream: [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium),
reference port layout [UniXNU/uefi-sheng](https://github.com/UniXNU/uefi-sheng).

## State

Early port. See [UniXNU-gauguin](https://github.com/lvyufeng/UniXNU-gauguin) for
the device repository and the bring-up order
([docs](https://unixnu.com/porting/bring-up)).
