# topaz-woa

Windows on ARM for **Redmi Note 12 4G** (`topaz` / `tapas`, Qualcomm SM6225 "khaje").

Status (2026-10-01): Windows 11 ARM64 (22621) boots to the desktop via [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) UEFI with the patches in `uefi/`.

## Layout
| Path | What |
|---|---|
| `uefi/patches/` | Mu-Silicium patches: topaz memory map (**required**), EnhancedFat fixes, optional USB-off |
| `uefi/mu_v4pack.sh` | Repack Mu image to boot header v4 (Xiaomi ABL ignores v1) |
| `drivers/TopazTouch/` | FocalTech touchscreen: TLMM + GCC + GENI I2C (polled) → VHF HID touch screen |
| `tools/` | canary, BCD-SYS helper, unattend.xml |

## Hardware notes
| Block | Address / pins |
|---|---|
| TLMM | 0x400000, tiles WEST 0x500000, SOUTH 0x900000, EAST 0xD00000, 0x1000/pin |
| GCC | 0x1400000, QUP0 vote reg 0x7900C (bits 6,7,8,9 + SE bit: s0=10, s1=11, s2=12) |
| QUP0 SE1 I2C | 0x4A84000, GPIO 4/5: bq2589x@0x6A (charger, OTG boost), rt1711h@0x4E (Type-C) |
| QUP0 SE2 I2C | 0x4A88000, GPIO 6/7 (func 1): FocalTech touch @0x38, IRQ GPIO80, RESET GPIO86, AVDD GPIO36 |
| Display FB | 0x5C000000, 1080x2400 XRGB8888 (continuous splash) |

## Driver install (test-signed)
Enable test signing in BCD (`testsigning` = element `16000049`), then
`pnputil /add-driver TopazTouch.inf` and `devcon install TopazTouch.inf Root\TopazTouch`.
Driver writes a log to `C:\TopazTouch.log`.
