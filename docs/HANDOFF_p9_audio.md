# Handoff: sound (P9) — state on 2026-10-02 evening

Read this first, then `docs/P9_audio.md` (all facts, logs, plan). Branch `audio`, worktree
`/Users/makr/Documents/topaz/topaz-woa-audio` (another agent works on GPU on `display` in
`../topaz-woa`; never touch TopazGpuW / TopazDisplay / TopazWifi). User language: Russian (they ask
for replies in Russian). Never give time estimates. User allows reinstalling TopazAudio and
rebooting the phone without asking (2026-10-02).

## Working with the phone
- `ssh topaz-win` (admin PowerShell). Complex scripts: scp a .ps1 to `C:\topaz\stage\` and run
  `powershell -NoProfile -ExecutionPolicy Bypass -File ...` (dot-sourcing in one-off ssh commands is
  blocked by the execution policy). PowerShell aliases `r`, `rd` shadow functions → helpers are
  `Get-Reg` / `Set-Reg` in `tools/audio/lab/lab.ps1` (copy it to `C:\topaz\stage\lab.ps1`).
- Install a TopazAudio build: `gh run download <run> -R MakrSas/topaz-woa -n TopazAudio-arm64`
  (artifact has `TopazAudio/` + `taudio/taudio.exe`) → scp to `C:\topaz\stage\TopazAudio\` →
  **certutil -addstore -f Root / TrustedPublisher topaz-woa-test.cer every time** (new cert per CI
  run) → `devcon.exe update TopazAudio.inf Root\TopazAudio`.
- Unloading TopazAudio stops the ADSP; the next ADSP boot needs fresh SMEM → **arm + restart**:
  `Set-Content C:\topaz\audio.arm on; shutdown /r /t 3`. A normal Windows restart is enough (no
  full power-off). ADSP is READY ~50 ms after boot, APM SPF ready ~1 s (log `C:\TopazAudio.log`).
  Windows password is off (auto-login), SSH uses a key.
- **Danger:** reading unknown LPASS registers hangs the bus → watchdog reboot (happened at
  0x0A7E0100 and on whole 0x0A6A9000 / 0x0A7A0000 pages). Lab MMIO is restricted to known blocks
  (`drivers/TopazAudio/Lab.c` MmioAllowed). Only read registers documented in DT / drivers.

## What exists
- `drivers/TopazAudio` (v0.5.4): ADSP PAS boot, SMP2P (items in 443 / out 429), GLINK (IPCRTR +
  adsp_apps; remote intent table 256), QRTR + pd-mapper, GPR, amp probe, **lab interface**
  `\\.\TopazAudio` (raw GPR send/recv, MMIO allowlist, I2C on QUP0 SE1 in TopazBattery quiet
  windows, contiguous buffers, PMIC read via SPMI observer).
- `tools/audio/taudio.c` (built in CI): `state | recv | cmd | apm | prm-hw | prm-clk | rd | wr |
  i2c | pmic`. Lab scripts `tools/audio/lab/`: `bringup.ps1` (votes, clocks, pins, macros, both
  SoundWire masters, codec reset), `graph1.ps1` / `graphstart.ps1` / `graphstop.ps1`.

## Results so far
1. ADSP + AudioReach up (A1-A3 done). Amp = **sia8159 @0x2b** on QUP0 SE1 (A4 done; reg map and
   on/off sequence in P9_audio.md).
2. PRM: DCODEC HW vote (id 2) OK; LPASS core vote (id 1) and PARAM_ID_RSC_LPASS_CORE fail
   (status 1); LPR (3) unsupported. All codec clocks (0x30e/0x30f 22.5792 MHz, 0x30c/0x30d/0x307/
   0x308 19.2 MHz) return 0.
3. LPASS macros readable/writable after the votes; both SoundWire masters (RX 0xa610000, VA
   0xa740000) report v1.6.0. VA master is clocked by the **VA macro** SWR gate (0xa730008), not by
   the TX macro (agatti differs).
4. **Blocker:** WCD937x never attaches on either SoundWire bus. Masters say frame-gen enabled but
   the bus clock never appears on the LPI pad (gpio3 / gpio0 IN bit stays 0; it does follow the pad
   when driven as GPIO), commands come back "ignored", bus-clash IRQ bit 3. Ruled out: LPI pin
   config/slew/OE/bits 10-11, codec reset (TLMM gpio92 high), L9 supply on, fs-gen, RX mclk muxsel,
   NPL clocks, init order, downstream swrm_master_init sequence, CGCR (HCTL) bits.
5. **AudioReach graph works:** a one-module graph (CODEC_DMA_SINK 0x07001023, iid 0x7001, SG 0x4001,
   container 0x4101 EP cap) on RX_CODEC_DMA_RX_0 opens, takes PARAM_ID_HW_EP_MF_CFG and
   PARAM_ID_CODEC_DMA_INTF_CFG (send params one per SET_CFG — several in one packet need 8-byte
   alignment per block, mine failed with status 1), PREPARE and START → status 0. It does not change
   the SoundWire state.

## Next steps (in order)
1. **Done: L14 (WCD937x VDD_BUCK) is OFF** (`taudio pmic 1 0x4d46` = 00, status 0x4d08 = 03, vset
   0x0708 = 1.80 V); L9 is on (80 / 87 / 1.80 V). Mainline wcd937x enables vdd-buck at probe; it is
   needed for AUX/HPH output in any case and may be why the codec does not attach. L14 belongs to
   RPM: apps must not write PMIC registers → write an **RPM SMD client** (GLINK over the RPM message
   RAM 0x045F0000 + 0x7000, channel "rpm_requests", APCS IPC bit 0, Linux qcom_glink_rpm.c +
   smd-rpm.c + qcom_smd-regulator.c: resource "ldoa" id 14, keys "swen"=1 / "uv"=1800000), vote L14
   on, then rerun bringup.ps1. The GLINK code in TopazAudio/Glink.c can be adapted (different FIFO
   location: TOC in message RAM).
2. Find why the SWR clock does not leave the macro: compare with a working mainline AudioReach +
   WCD937x board (qcm6490 IDP: `audioreach-topology/QCM6490-IDP.m4`, Linux sc7280 lpass DT) and
   the downstream bengal bolero code (sources fetched from NothingOSS / Pzqqt marble trees — see
   P9_audio.md). Suspects: a clock only the DSP can enable (LPASS core vote fails), an LPASS
   "swr clk" enable not in the macro registers.
3. After the codec attaches: WCD937x init (downstream wcd937x.c), AUX path, sia8159 on, data path
   via WR_SHARED_MEM_EP (needs apps SMMU SID 0x1C1 identity bank, like Wi-Fi's 0x1A0) → first tone,
   then an ACX endpoint.
