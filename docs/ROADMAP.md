# Roadmap: Windows on Redmi Note 12 4G (topaz) — 2026-10-01

What works, what is being done now, and the plan for the remaining hardware. Hardware facts marked
**DT** come from the stock device tree (s8build `~/work/topaz/backup/fdt.dts`). Items marked
**hypothesis** still need checking on the phone.

Costs are in working hours, calibrated on Wi-Fi: from the first research note to internet in
Windows took **one day** (2026-10-01, 03:28 → 22:08: modem boot, GLINK/QRTR, WLFW, CE/HTC/WMI/HTT,
scan, association, WPA2, data path), the WiFiCx driver itself **3.5 hours** (v0.1 18:33 → working
22:08). Most of that time was test loops through the flash drive; SSH makes them shorter. What can
blow an estimate up is not the amount of code but a hardware lock (TZ/XPU, SMMU) or firmware that
wants something undocumented — each item lists its main unknown.

## Works today
| Block | Driver | Notes |
|---|---|---|
| Boot, UFS, CPU, display (UEFI framebuffer) | Mu-Silicium + `uefi/` patches, `TopazDisplay` (KMDOD) | no GPU acceleration |
| USB host (hub, mouse, keyboard, flash drive) | `TopazOtgDxe` + XHCI DSDT | |
| Touchscreen (FT5452, 10 fingers) | `TopazTouch` | polled |
| Buttons Power / Vol- / Vol+ | `TopazButtons` | PMIC over the SPMI observer |
| Battery, charging (~2 A at 5 V), Type-C roles | `TopazBattery` | `docs/NOTES_power.md` |
| Modem boot (MPSS), GLINK/QRTR, rmtfs | `TopazModem` core | base for Wi-Fi and SIM |
| **Wi-Fi** (scan, WPA2, internet) | `TopazWifi` (WiFiCx) | legacy 54 Mb/s for now |

## In progress
- **SSH into the phone** (`tools/deploy/enable-ssh.cmd`): the build host installs drivers, reads
  logs and reboots over Wi-Fi without the flash-drive loop.
- **Wi-Fi P6: HT/VHT** (802.11n/ac): plan in `docs/HANDOFF_p6_htvht.md`. Then P7: CE interrupts
  instead of 1 ms polling, real MAC, statistics, roaming, WPA3.

## Planned
| # | Item | Shared base | Cost | Main unknown |
|---|---|---|---|---|
| 0 | **CPU frequency** (no DVFS in Windows: no Qualcomm PEP/PPM) | — | read the level ~1 h; fixed max level ~1-2 h; own load-based governor a few hours | does the cpufreq-hw block accept apps writes |
| 1 | Screen rotation (accelerometer / gyro) | display rotation; ADSP if the IMU sits behind it | display ~1 h; sensor: a few hours direct, ~1 day via ADSP | is the IMU bus open to apps |
| 2 | SIM: mobile internet (**no calls**) | running modem + QRTR (already have) | QMI (SIM, network, data call) a few hours; IPA data path ~1 day; "Cellular" page ~1 day | IPA/GSI bring-up |
| 3 | Sound (speaker, headphones) | ADSP boot + AudioReach | ADSP boot a few hours (PAS + GLINK code exists); graph + ACX 1-2 days | AudioReach graph / calibration |
| 4 | Microphone | same stack as sound | a few hours after sound | — |
| 5 | GPU (Adreno 610) | — | Qualcomm-driver experiment a few hours; brightness / panel off a few hours; own driver: the biggest item, weeks | Qualcomm drivers expect a GMU |
| 6 | Camera (maybe) | — | raw frames from one sensor 1-2 days; a decent picture: more | sensor init tables, no ISP tuning |

Suggested order: rotation (cheap, visible) → SIM data (the modem base is done) → ADSP → sound → mic.
The GPU experiment and display improvements fit in between; camera last.

## 0. CPU frequency
- Measured 2026-10-01 over SSH while the UI lagged: 65% total CPU with the VS Code installer running,
  `dwm` alone ~2 of 8 cores (software composition, no GPU). Windows reports 908 MHz (perf counter) /
  1517 MHz (WMI) for a 2.8 GHz-class SoC — not trustworthy, but nothing in this port drives DVFS, so
  the cores most likely stay at the bootloader's level.
- DT: `qcom,cpufreq-hw` at 0xF521000 (silver, domain 0) and 0xF523000 (gold, domain 1), 12 LUT
  entries, LMh attached (hardware thermal limit). Linux `qcom-cpufreq-hw.c` (v1 layout): ENABLE 0x0,
  FREQ_LUT 0x110 / VOLT_LUT 0x114 (32-byte rows), PERF_STATE 0x920 (the level to run at).
- Plan: log the LUT and PERF_STATE from a driver (read only) → set the top level for both domains
  (voltage and thermal throttling are handled by the OSM/LMh hardware) → later a simple governor
  from CPU load (and idle states / PEP for battery).

## 1. Screen rotation
- Windows auto-rotation needs only an **accelerometer** (gyro is optional). Two parts:
  1. **Display**: `TopazDisplay` (from the Microsoft KMDOD sample) advertises Identity + Rotate90 only
     (`bdd_dmm.cxx`). Add 180/270 in the rotation support and the blit path. Cost: ~1 hour.
  2. **Sensor**: no accel/gyro node on the apps I2C buses in the DT; the DT has
     `qcom,fastrpc-adsp-sensors-pdr` → **hypothesis:** the IMU is owned by the ADSP sensor framework
     (SEE). First step: read `/vendor/etc/sensors/config/*.json` (TWRP, read-only) for the chip
     model and its bus / QUP SE.
- Path A (if the IMU bus is reachable from apps): read the IMU directly over I2C/I3C (polled), expose
  it with SensorsCx as an accelerometer → Windows rotates by itself. Risk: the bus is locked to
  the ADSP by TZ (XPU) → access faults.
- Path B: boot the ADSP (TZ PAS like the modem) + its GLINK edge + QRTR, then a QMI sensor client
  (SEE, protobuf; open reference: postmarketOS `libssc` + `hexagonrpcd`, which also serves the sensor
  registry over FastRPC). Bigger, but the ADSP boot is reused by sound.

## 2. SIM: mobile internet (calls are out of scope)
- Already have: MPSS running, QRTR, rmtfs serving the EFS, so modem QMI services are reachable.
- Steps:
  1. QMI clients in TopazModem, log only: DMS (IMEI, online mode), UIM (SIM state, PIN),
     NAS (registration, signal), WDS (start a data call with an APN).
  2. Data path: **IPA v4.2** (DT: `qcom,ipa@0x5800000`, `qcom,ipa-hw-ver = <0x10>`) + GSI. Linux
     mainline `drivers/net/ipa` supports v4.2 (sc7180) = the open reference. IPA has its own SMMU
     context banks (`ipa-smmu-ap-cb`) → same identity-bank trick as Wi-Fi. Find out who loads the
     GSI firmware (the modem or the apps via PAS; mainline `modem-init`).
  3. Windows, stage A: a plain NetAdapter (raw IP) → internet shows up like a wired connection, APN
     from a config file. Stage B: MBBCx (Mobile Broadband class extension) = the real "Cellular"
     page (operator, signal bars, APN, SIM PIN), translating MBIM ↔ QMI.
- SMS: not planned; cheap to add after stage B if wanted.
- Risks: IPA/GSI bring-up is the big unknown; the modem may need the right carrier config (MBN).

## 3. Sound (speaker, headphones)
- Hardware (DT):
  - LPASS "Bolero" digital codec (VA / RX / TX macros) + **WCD937x** analog codec over SoundWire;
  - speaker amp on **QUP0 SE1 I2C** (the bus TopazBattery already drives), three alternatives in
    the DT: `fs16xx` @0x34, `aw87xxx` @0x59, `sia81xx` @0x2b, shared reset GPIO 106 → an I2C probe
    tells which one is fitted (aw87xxx / sia81xx are analog amps fed by the codec);
  - `fsa4480` @0x42 = USB-C audio switch;
  - sound card `qcom,bengal-asoc-snd` under `spf_core_platform` = **AudioReach on the ADSP**
    (GPR / q6prm over the ADSP GLINK edge).
- Path: ADSP boot (shared with sensors path B) → GPR over GLINK → AudioReach graph (PCM → codec DMA
  → RX macro → SoundWire → WCD937x → amp). Reference: Linux `sound/soc/qcom/qdsp6` (q6apm,
  audioreach) + topology files. Windows side: ACX (Audio Class eXtension) render endpoint.
- First milestone: ADSP up and GPR answering. Risks: graph/topology for this SoC, calibration (ACDB).

## 4. Microphone
- Same stack, capture side: TX / VA macros + WCD937x ADCs (or DMIC) → AudioReach capture graph →
  ACX capture endpoint. Comes after sound works.

## 5. GPU (Adreno 610v2)
- DT: `qcom,kgsl-3d0@5900000`, chipid 0x6010001, "Adreno610v2". Without a GPU driver Windows draws in
  software; the basic display driver is also a Modern Standby blocker (`powercfg /a`).
- Qualcomm ships Windows Adreno drivers only for 7c / 8cx / X Elite (Adreno 618 / 680 / 690 / X1).
  Adreno 610 has **no GMU** (Linux drives it with a "GMU wrapper"), those drivers expect one →
  reuse is unlikely, but an experiment with a 7c (Adreno 618) package costs a few hours (ACPI
  entries + driver package). Own driver = the biggest item on this list (weeks): WDDM kernel part
  (memory manager, ring buffer, A6xx init without GMU — Linux msm/Freedreno as the hardware
  reference) + a D3D user-mode driver; a display-only + compute path could come first.
- Cheap display work in `TopazDisplay` meanwhile: rotation (see 1), **brightness** via MIPI DCS
  0x51 through the MDSS DSI host (AMOLED, video mode) → Windows brightness slider, panel off/on
  (DCS 0x28/0x10) for sleep, vsync from MDP interrupts.

## 6. Camera (maybe)
- Hardware (DT): 4 sensors on CCI0/CCI1 (cell-index 0-3), CSIPHY 0-2, CSID 0-2, TFE 0-2 (thin front
  end), OPE; camera LDO `wl2866d` @0x28 on QUP0 SE1 I2C; MCLK 0-3.
- Path: power (LDO, MCLK, reset GPIOs) → sensor init over CCI (register tables from the Android
  vendor camera modules) → CSIPHY/CSID/TFE raw Bayer into memory → Windows camera (AVStream) driver
  with software debayer and our own crude auto-exposure. Front camera first (one sensor, no AF).
- Raw frames from one sensor: 1-2 days; the picture quality will be far from Android (no ISP
  tuning) unless we also write auto-exposure / white balance / denoise.

## Not in the list (candidates)
Bluetooth (same WCN3950 chip as Wi-Fi), real sleep / Modern Standby (`docs/NOTES_power.md`),
USB-PD 9 V charging, fingerprint (TEE-bound, unlikely).
