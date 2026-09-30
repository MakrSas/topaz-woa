# Brief: Windows drivers for Redmi Note 12 4G (topaz, SM6225)

You own **Windows drivers** in `drivers/`. The lead agent owns UEFI, boot chain,
partitioning, flashing and the phone's boot slots. Read this whole file first.

## 1. Where things are (2026-10-01)
- Windows 11 Pro ARM64 **22621**, ru-RU, user `makr` (no password, autologon), boots to
  desktop through Mu-Silicium UEFI loaded from RAM (`fastboot boot`). Display = UEFI
  framebuffer (1080x2400), UFS works, CPU works. **No input devices at all** (no touch,
  no buttons, USB host has no VBUS). So every test is "deploy offline → boot → read logs offline".
- Test signing is ON in BCD (`16000049` on loader `{68d01361-c243-4f1a-b850-5681ad4345e2}`).
- `drivers/TopazTouch` v0.1 is written, **builds in CI, not yet tested on hardware**.
  It is a root-enumerated KMDF driver that maps TLMM/GCC/QUP registers directly
  (`MmMapIoSpaceEx`), runs GENI I2C in polled FIFO mode and exposes a 10-finger
  touch screen via VHF. It logs to `C:\TopazTouch.log`.
- No SoC drivers exist for SM6225 anywhere. SM6225 uses **RPM** (not RPMh), so Qualcomm
  reference drivers for 7c/8cx power/clocks do not apply. Prefer small self-contained
  drivers that touch registers directly; ACPI can come later.

## 2. Hardware (verified from the phone's DTB + MiCode `topaz-t-oss` kernel)
| Block | Details |
|---|---|
| TLMM | base 0x400000; tiles WEST 0x500000, SOUTH 0x900000, EAST 0xD00000; pin regs at tile+0x1000*gpio; CTL: pull[1:0] func[5:2] drv[8:6] oe[9]; IO: in[0] out[1] |
| GCC | 0x1400000; QUP0 vote reg 0x7900C bits 6(m_ahb) 7(s_ahb) 8(core) 9(core2x) 10(s0) 11(s1) 12(s2); RCG s1 0x1F278, s2 0x1F3A8 (src 0 = XO 19.2 MHz) |
| QUP0 SE1 I2C | 0x4A84000, GPIO 4/5 (func qup1). **bq2589x @0x6A** (charger, OTG boost), **rt1711h @0x4E** (Type-C TCPC, INT GPIO 93), **sm5602 @0x71** (fuel gauge), sc8551 / ln8000 (charge pumps) |
| QUP0 SE2 I2C | 0x4A88000, GPIO 6/7 (func 1 = qup2), 2 mA, no bias. **FocalTech @0x38** (`focaltech,fts`, TDDI with panel `m7_38_0c_0a`), IRQ GPIO80 (pull-up, active low), RESET GPIO86, AVDD GPIO36 (EAST, output high) |
| Keys | Power = PM6125 PON KPDPWR, Vol- = PM6125 PON RESIN (SPMI arbiter 0x1C40000), Vol+ = PM6125 GPIO5 (pull-up) |
| USB | DWC3 0x4E00000 (dr_mode otg). VBUS for OTG must come from bq2589x boost + rt1711h source role |
| Display | continuous-splash FB 0x5C000000, XRGB8888 |

Useful sources (already downloaded on the laptop): `~/work/topaz/backup/fdt.dts` (full DTB),
MiCode kernel branch `topaz-t-oss` (github.com/MiCode/Xiaomi_Kernel_OpenSource) for
`gcc-khaje.c`, `pinctrl-khaje.c`, `qcom-geni-se.c`, `i2c-qcom-geni.c`. The touch/charger
vendor drivers are **not** in that tree (vendor modules). The phone's own `.ko` files and
touch firmware are on the phone (vendor_dlkm / vendor) — pull them if needed.

## 2b. Touch — WORKS (2026-10-01)
Installed manually from a flash drive (`tools/deploy/install-manual.cmd`, admin). v0.2 log:
chip_id 0xA3=0x54, 0x9F=0x52 -> **FocalTech FT5452**, fw 0x36, vendor 0x48; answers before
reset -> has its own flash, no fw upload needed. TLMM at driver start: gpio6/7 func 1,
irq80 ctl=0xC3 (pull-up, 8 mA), rst86/avdd36 output high. GCC vote already 0x5BE0.
Classic 12-bit FT decode is right, but range is exactly 1.25x the panel: corner taps give raw
x 0..1349, y 0..2999 (y saturates at 2999) on 1080x2400 -> v0.4 uses HID logical max 1349x2999.
Edge contacts sometimes come with id 9. The fw repeats an "up" slot for many
frames -> report the lift once. v0.1 log path `\SystemRoot\..` never worked (use `\??\C:\`).

## 2a. USB host — WORKS (2026-10-01)
Mouse through a bus-powered hub works in Windows with `Mu-topaz-v4-OTG3-RELEASE.img`
(`~/work/win/uefi/`). Three pieces, all in UEFI (RAM boot, nothing flashed):
1. Qualcomm USB DXEs enabled (tapas `UsbConfigDxe.patched.efi` already takes the host path
   for platform type 0x22 = IDP; board-id 0x30022). DEBUG builds spam
   `UsbPwrCtrlLib_GetVbusSourceOK Failed` forever (UsbPwrCtrlDxe polls a PMIC charger that
   doesn't exist) — use RELEASE.
2. `uefi/TopazOtgDxe` (tapasPkg driver): raw GENI I2C on QUP0 SE1, enables OTG boost on the
   charger (0x6A). At ReadyToBoot it prints its log and waits up to 60 s for the PC cable to be
   swapped for the hub: **the charger drops OTG_CONFIG while VBUS has an external supply**
   (REG0B VBUS_STAT=1), so OTG can only be set after unplugging the PC.
3. DSDT: `URS0`(QCOM0497/PNP0CA1)+`USB0`+`UFN0` replaced by a plain XHCI `USB0` (`PNP0D10`,
   0x4E00000, GSIV 0x11F) — `uefi/patches/tapas-dsdt-xhci.diff`. With URS0 Windows' inbox
   `urssynopsys` picked the FUNCTION role (no ID pin) and never exposed the host.

Charger facts: REG14=0x4C (PN=001: not a genuine bq25890, likely SY6970/SC89890 clone, same
register map), REG07=0x8D (watchdog already off), REG0A=0x73 (5.126 V / 1.4 A boost),
REG03=0x1A. rt1711h VID/PID 29CF/1711, ROLE_CONTROL=0x0A (Rd/Rd). QUP0 SE1: fw proto 3 (I2C),
tx_depth 16, DFS level 0 = XO (perf 0x1), CLK_SEL 0. USB D+/D- switch GPIO66 = output low (SoC).

Windows clock is 2022-05-07 (no RTC): test certs must have NotBefore in the past
(TopazTouch install failed with 0x800B0101 because of this; fixed in CI).

## 3. Build
Push to `main` (or a branch) → GitHub Actions `Build drivers` (windows-2022, WDK 26100)
builds ARM64 Release, stamps INF, makes the catalog, **test-signs with a fresh
self-signed cert** and uploads artifact `TopazTouch-arm64`
(`.sys .inf .cat topaz-woa-test.cer devcon.exe`). Download: `gh run download <id> -n TopazTouch-arm64`.
Notes: InfVerif and ApiValidator are broken on the runner and are disabled; no `__DATE__`
(deterministic build); `ZwFlushBuffersFile` is not in ntddk.h.

## 4. Deploy / test loop (no input in Windows!)
The phone is reached **only through the laptop**: `ssh s8build` (then `adb`/`fastboot`).
- Install path that works: files in `C:\topaz\drivers\<Name>\`, script `C:\topaz\install.cmd`
  run at boot as SYSTEM by a **local GPO startup script**
  (`C:\Windows\System32\GroupPolicy\gpt.ini` + `Machine\Scripts\scripts.ini`, templates in `tools/deploy/`).
  The script is one-shot (`C:\topaz\installed.flag`); delete the flag to re-run.
  It adds the cert to Root+TrustedPublisher and runs `devcon install … Root\<HWID>`.
- To copy files you must mount NTFS from TWRP: `mount.ntfs /dev/block/by-name/win /mnt/win`.
  **DANGER**: after a hard reset Windows' NTFS journal is dirty and ntfs-3g refuses RW.
  **Never run `ntfsfix` on it** — clearing the journal corrupted the volume once
  (BSOD NTFS_FILE_SYSTEM, Windows had to be re-applied). Mount **read-only** (`-o ro`)
  to read logs; only write when Windows was shut down cleanly (fast startup is disabled
  in the current install). If a write is unavoidable, ask the lead.
- `adb push` straight onto ntfs/vfat mounts is unreliable: push to `/tmp` then `cp`.
- Boot Windows: phone in bootloader fastboot (not fastbootd: `fastboot getvar is-userspace` must be `no`),
  then `fastboot boot ~/work/win/uefi/Mu-topaz-v4-CLEAN-NOUSB-RELEASE.img`.
  The UEFI is not flashed yet: any Windows reboot lands in Android/fastboot and the
  UEFI must be booted again.

## 5. Rules
1. **One agent drives the phone at a time.** Before any adb/fastboot command, tell the
   user and wait if the lead is using it. Never flash partitions, never touch GPT,
   `boot_*`, `vendor_boot_*`, `dtbo_*`, `abl/xbl/tz/hyp`, `persist`, `modemst*` — that is the lead's area.
2. Work in branches + PRs to `MakrSas/topaz-woa`; keep `main` building.
3. Every driver must log to a file under `C:\` (offline debugging is the only debugging).
4. Map only the MMIO you need; do not touch GCC/TLMM registers of other blocks.
5. Hardware facts you discover go into `README.md` / this file.

## 6. Task list (priority order)
1. **Touch** — test TopazTouch v0.1, read `C:\TopazTouch.log`. Open questions: does the
   FocalTech TDDI keep its firmware after UEFI (chip id at 0xA3), or must the driver
   upload firmware on every power-up (flash-less TDDI)? Coordinates are 12-bit in the
   classic FT protocol but DTB says `display-coords 0..10799 x 0..23999` (hi-res mode) —
   check raw logs. Then: IRQ-driven instead of polling, proper ACPI device later.
2. **Buttons** — Vol+/Vol-/Power as a HID keyboard/consumer device (needs a minimal
   SPMI read path to PM6125 PON + PM6125 GPIO5).
3. **USB OTG** — enable bq2589x OTG boost, rt1711h source role, DWC3 host mode so
   Windows' inbox XHCI works (hub + keyboard). Coordinate with the lead: may end up in UEFI.
4. **Battery** — sm5602 fuel gauge → battery miniclass (percentage in taskbar).
5. **Wi-Fi/BT** (WCN3990 behind MPSS) — research only.
