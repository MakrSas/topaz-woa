# topaz-woa

Windows on ARM for **Redmi Note 12 4G** (`topaz` / `tapas`, Qualcomm SM6225 "khaje").

Status (2026-10-01): Windows 11 ARM64 (22621) boots to the desktop via [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) UEFI with the patches in `uefi/`.

| Feature | State |
|---|---|
| Boot to desktop (UEFI from RAM via `fastboot boot`) | works |
| Display (UEFI framebuffer), UFS, CPU | works |
| **USB host** (bus-powered hub, mouse, keyboard, flash drive) | **works** — `TopazOtgDxe` + XHCI DSDT |
| **Touchscreen** (FocalTech FT5452, 10 fingers) | **works** — `TopazTouch` v0.4 (polled) |
| Buttons (Power, Vol-, Vol+) | works — `TopazButtons` |
| Battery, charging, Type-C roles | works — `TopazBattery` |
| **Wi-Fi** (scan, WPA2, internet) | **works** — `TopazWifi` (legacy 54 Mb/s for now) |
| Rotation, SIM data, sound, mic, GPU, camera, BT | not yet — see [docs/ROADMAP.md](docs/ROADMAP.md) |

How to boot and install drivers: [docs/WINDOWS_USB_AND_INSTALL.md](docs/WINDOWS_USB_AND_INSTALL.md).

## Layout
| Path | What |
|---|---|
| `uefi/build_topaz_uefi.sh` | One-shot build of the working UEFI image (applies everything below, builds, repacks v4) |
| `uefi/patches/` | Mu-Silicium patches: topaz memory map (**required**), EnhancedFat fixes, TopazOtgDxe hookup, DSDT diff |
| `uefi/TopazOtgDxe/` | UEFI DXE: charger OTG boost (5 V VBUS for USB host) over raw GENI I2C |
| `uefi/acpi/` | DSDT with plain XHCI `USB0` (`.dsl` source + compiled `.aml`) |
| `uefi/mu_v4pack.sh` | Repack Mu image to boot header v4 (Xiaomi ABL ignores v1) |
| `drivers/TopazTouch/` | FocalTech FT5452 touchscreen: TLMM + GCC + GENI I2C (polled) → VHF HID touch screen |
| `tools/deploy/install-manual.cmd` | Self-elevating installer for a driver package on a flash drive |
| `tools/` | canary, BCD-SYS helper, unattend.xml |

## Hardware notes
| Block | Address / pins |
|---|---|
| TLMM | 0x400000, tiles WEST 0x500000, SOUTH 0x900000, EAST 0xD00000, 0x1000/pin |
| GCC | 0x1400000, QUP0 vote reg 0x7900C (bits 6,7,8,9 + SE bit: s0=10, s1=11, s2=12) |
| QUP0 SE1 I2C | 0x4A84000, GPIO 4/5 (func 1): charger @0x6A ("bq2589x", REG14=0x4C = PN 001, bq25890-compatible clone), rt1711h @0x4E (VID/PID 29CF/1711), fsa4480 @0x42 |
| Charger OTG | REG03 bit5 OTG_CONFIG; **refused while VBUS is externally powered** (REG0B VBUS_STAT=1); VBUS_STAT=7 = boost on. Watchdog already off (REG07=0x8D), boost 5.126 V/1.4 A (REG0A=0x73) |
| USB | DWC3 0x4E00000, core IRQ SPI 0xFF (GSIV 0x11F), high-speed only; D+/D- switch GPIO66 low = SoC; host mode set by UEFI `UsbConfigDxe` (tapas patch: platform type 0x22 IDP) |
| QUP0 SE2 I2C | 0x4A88000, GPIO 6/7 (func 1): **FocalTech FT5452** @0x38 (chip id 0x54/0x52, fw 0x36, own flash), IRQ GPIO80, RESET GPIO86, AVDD GPIO36 |
| Touch coords | classic 12-bit FT format, raw range **0..1349 x 0..2999** = 1.25x the 1080x2400 panel |
| QUP clocks | RCGs in DFS mode; DFS level 0 = XO 19.2 MHz (`SE_GENI_CLK_SEL`=0); fw proto 3 = I2C, tx_depth 16 |
| Display FB | 0x5C000000, 1080x2400 XRGB8888 (continuous splash) |

## Driver install (test-signed)
Test signing is on in BCD (element `16000049`). Copy the CI artifact `TopazTouch-arm64` to a flash
drive with `tools/deploy/install-manual.cmd` and run it (asks for admin). Windows boots with its clock
at 2022-05-07 — CI certs are valid from 2015 for that reason. Driver log: `C:\TopazTouch.log`.
