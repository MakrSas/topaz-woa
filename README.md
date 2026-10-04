# topaz-woa

Windows on ARM for **Redmi Note 12 4G** (`topaz` / `tapas`, Qualcomm SM6225 "khaje").

Status (2026-10-04, evening): Windows 11 ARM64 (22621) boots to the desktop via [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) UEFI with the patches in `uefi/`.
The desktop can be composed by **DWM on the Adreno 610 GPU** (see [GPU](#gpu-adreno-610) below).

| Feature | State |
|---|---|
| Boot to desktop (UEFI from RAM via `fastboot boot`) | works |
| Display (UEFI framebuffer), UFS, CPU | works |
| **GPU (Adreno 610), DWM hardware composition** | **works** — `TopazGpuW` (WDDM KMD) + Mesa freedreno/d3d10umd UMD, needs the GPU0 UEFI image |
| Brightness slider | works — through the display/GPU adapter |
| CPU clocks (both clusters at max) | works — `TopazCpu` (`C:\topaz\cpu.max`) |
| **USB host / OTG** (bus-powered hub, mouse, keyboard, flash drive) | **works** — `TopazOtgDxe` + XHCI DSDT |
| **Touchscreen** (FocalTech FT5452, 10 fingers) | **works** — `TopazTouch` (polled, ~150 reports/s) |
| Buttons (Power, Vol-, Vol+) | works — `TopazButtons` |
| Battery, charging, Type-C roles | works — `TopazBattery` |
| **Wi-Fi** (scan, WPA2, internet) | **works** — `TopazWifi` (legacy 54 Mb/s for now) |
| **Speaker** (Windows sound device, apps + volume slider) | **works** — `TopazAudio` (ADSP / AudioReach) + `TopazSpeaker` (ACX endpoint), [docs/P9_audio.md](docs/P9_audio.md) |
| RPM requests (rails, clocks) | works — `TopazRpm` (IPA clock, GPU CX level + DDR, codec buck L14) |
| SIM | SIM / LTE registration work; mobile data in progress ([docs/P9_sim.md](docs/P9_sim.md)) |
| OpenGL / Vulkan apps | not yet (D3D10/11 only) — plan in [docs/P10_opengl_vulkan.md](docs/P10_opengl_vulkan.md) |
| Rotation | portrait only on the GPU path (rotation attempt v0.46-v0.49 dropped); worked on the plain display driver |
| Headphones jack, mic, camera, BT, Modern Standby | not yet — see [docs/ROADMAP.md](docs/ROADMAP.md) |

Android stays on slot A (dual boot by slot): rules for flashing your own ROM there without breaking
Windows in [docs/ANDROID_SLOT_A.md](docs/ANDROID_SLOT_A.md).

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
| `drivers/TopazGpuW/` | WDDM display + render KMD for the Adreno 610 (power-up, zap, SMMU, ring, msm-style escapes, MDP scanout) |
| `mesa-overlay/` | Mesa UMD sources/patches (freedreno + d3d10umd + WDDM winsys), built by `.github/workflows/mesa.yml` |
| `drivers/TopazAudio/` | ADSP boot (PAS), SMP2P/GLINK/QRTR/GPR to AudioReach, lab interface `\\.\TopazAudio` (GPR, MMIO, I2C) |
| `drivers/TopazSpeaker/` | ACX 1.0 speaker endpoint: codec bring-up, pull-mode DSP graph, volume (talks to TopazAudio) |
| `drivers/TopazRpm/` | RPM GLINK client: IPA clock, GPU CX/DDR votes, L14, LPI rails (opt-in flags) |
| `drivers/TopazDisplay/`, `TopazWifi/`, `TopazModem/`, `TopazBattery/`, `TopazButtons/`, `TopazCpu/` | the other platform drivers |
| `tools/gpu/` | GPU test tools: `fpsbench.ps1`, `movebench.ps1`, `touchrate.ps1`, `hangwatch.ps1` |
| `tools/audio/` | `taudio` (lab CLI), `wasapitest`, `kstest`, `lab/*.ps1` (the scripted speaker bring-up) |
| `tools/deploy/install-manual.cmd` | Self-elevating installer for a driver package on a flash drive |
| `tools/` | canary, BCD-SYS helper, unattend.xml |

## Boot menu (UEFI)

`uefi/` builds the Mu-Silicium UEFI for boot_b: the stock Silicium boot screen, then a touch
**WinRE-look "Choose an option" page** (5 s countdown to Windows; Windows without GPU,
Fastboot, Power off; the back arrow opens the old text menu). The WinRE look, its icons and its fonts come from
**NTDEV's [exynos9810-woa](https://github.com/ntdevlabs/exynos9810-woa)** (`tools/twrp-winre`,
BSD-2-Clause-Patent; fonts SIL OFL 1.1, Selawik-based) — see [NOTICE.md](NOTICE.md).

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

## GPU (Adreno 610)

Windows has no Qualcomm GPU driver for this SoC (the Snapdragon 7c driver needs a GMU, the A610 has none), so
the stack is built from open pieces. Running notes: [docs/P8_gpu.md](docs/P8_gpu.md); handoff: [docs/HANDOFF_p8_gpu.md](docs/HANDOFF_p8_gpu.md).

### How it works
```
 DWM / apps (D3D11 on FL10)
   │  D3D10 DDI
 topazgpu_d3d10.dll  = Mesa d3d10umd (gallium frontend)
   │                 → tgsi_to_nir → ir3 compiler (Adreno shader ISA)
   │                 → freedreno a6xx gallium driver (command streams, GMEM/sysmem, blits)
   │                 → freedreno "msm" DRM backend over a WDDM winsys (fd_wddm.c):
   │                   every DRM ioctl (GEM_NEW, GEM_SUBMIT, WAIT_FENCE …) = one D3DKMTEscape
   │  pfnEscapeCb / pfnPresentCb
 TopazGpuW.sys (WDDM 2.x KMD, display + render)
   ├─ power: GPU CC GDSCs/clocks, gpu_cc_pll0 (Zonda) for the core clock, zap shader via TZ (PAS 13)
   ├─ identity SMMU context, CP ring, SQE microcode, CP_ME_INIT
   ├─ msm escapes: GEM BOs (contiguous, identity-mapped), submit (≤512 IBs), fences, CPU_PREP
   ├─ display: VidPN/mode set, flips = MDP VIG0 SRC0_ADDR → direct scanout of the DWM primary,
   │   flip done when the MDP latched it; 1 ms high-res timer for vsync / flip completion
   └─ async present: a flip waits in the KMD until the BO's GPU fence completed
```
- The GPU adapter exists only in the **GPU0 UEFI image** (ACPI `GPU0` device), RAM-booted with
  `fastboot boot Mu-topaz-v4-GPU0-RELEASE.img`; the flashed UEFI keeps the plain framebuffer driver
  (`TopazDisplay`), which yields the panel when GPU0 is present.
- UMD opt-in: `C:\topaz\umd.enable`. Debug switches for the UMD process environment: `C:\ProgramData\topaz\umd.env`
  (`NAME=VALUE` lines, e.g. `TOPAZ_TIMING=1`, `FD_MESA_DEBUG=…`); log `C:\ProgramData\topaz\umd-<pid>.log` when
  `umd.log.enable` exists. KMD log `C:\TopazGpuW.log` (an `fps:` line per second).
- GPU clock: `C:\topaz\gpu.mhz` = one of the stock OPPs (320 465 600 785 820 980 1025 1100 1260); the phone runs
  **1260** with `TopazRpm` + `C:\topaz\rpm.gpu` voting CX TURBO_L1 (rwcx 0x1a0) and the top DDR (bimc) level.
- KMD **v0.45.8**: screen off = backlight off (no frozen boot picture); vsync really 60 Hz (timer-callback tolerance + 1 ms timer resolution; it ran at 31-40 Hz),
  at most one flip completion per vsync, per-second `fps:` stats (shown / flips / vsync notifies).

### Fixes that made it usable (details in docs/P8_gpu.md)
| Problem | Cause | Fix |
|---|---|---|
| stale / zoomed window copies, clipped shadows | DWM rewrites constant buffers with UpdateSubresourceUP; freedreno did not see pending reads | flush before CB updates (`TOPAZ_UPDFLUSH`, to be replaced by CB renaming) |
| 1-px strokes, text fragments, diagonal seams | ir3 never enabled full 2x2 quads (helper lanes) for TGSI shaders → garbage derivatives | `lodpixmask`/`pixlodenable` always on (`TOPAZ_FULLQUAD`) |
| ~2300 BO alloc/free per second, slow drags | `bo_reuse` enum bitfield is signed under MSVC (`RING_CACHE` read back as -2) → ring BO cache dead | bitfield widened |
| ~11000 escapes/s | GEM_MADVISE on every cache put/get | answered in the winsys |
| flips at ~30 FPS | vsync = 16 ms KTIMER on 15.6 ms ticks, flip done one tick late | 1 ms high-res timer, flip done right after the MDP latch |
| CPU waits for the GPU every frame | Present did flush + fence wait | async present (KMD defers the flip to the fence) |
| explorer crashes | per-process global scratch state shared by several D3D devices | per-device state |
| everything ~33 FPS | vsync tick needed `now >= next` on ~15.6 ms timer callbacks → every second period | tick tolerance + 1 ms timer resolution (v0.45.1) |

### Known limits
portrait only; finger window drags with the blurred "lifted window" effect ~20 FPS (GPU-bound, mouse ~45);
Modern Standby (BSOD 0x14F) disabled via power timeouts; preemption off; only DWM/apps on FL10 (D3D10 DDI) -
no OpenGL ICD and no Vulkan driver yet (games asking for GL 2.1+/Vulkan refuse to start).
