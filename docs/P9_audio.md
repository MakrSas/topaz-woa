# P9: sound (ADSP + AudioReach) — branch `audio`, driver `drivers/TopazAudio`

Goal: speaker and headphones in Windows. Roadmap item 3 (`docs/ROADMAP.md`). Work happens in the
worktree `../topaz-woa-audio` on branch `audio` (another agent works on GPU on `display`).
Rules for this work: do not touch TopazGpuW / TopazDisplay / TopazWifi; never reboot the phone; ask
the user before installing anything that could take Wi-Fi/SSH down.

## Plan
| Step | What | State |
|---|---|---|
| A1 | ADSP boot via TZ PAS + SMP2P ready | v0.1 written, untested |
| A2 | GLINK lpass edge: IPCRTR (QRTR, pd-mapper), adsp_apps (GPR) | v0.1 written, untested |
| A3 | GPR: APM `GET_SPF_STATE` = 1 (AudioReach up) | v0.1 written, untested |
| A4 | Find the fitted speaker amp (I2C probe on QUP0 SE1: fs16xx 0x34 / aw87xxx 0x59 / sia81xx 0x2b) | |
| A5 | AudioReach graph for speaker: PCM shared mem → codec DMA RX → RX macro → SoundWire → WCD937x / amp | |
| A6 | Tone from the driver (no Windows audio stack) | |
| A7 | ACX render endpoint (Windows sees a speaker) | |
| A8 | Headphones (WCD937x HPH, fsa4480 USB-C switch), mic (TX/VA macros) | |

## Hardware facts (stock DT, `vendor_boot_a.img` SoC dtb "Khaje", extracted with dtc)
- `remoteproc-adsp@ab00000`, `qcom,khaje-adsp-pas` → **PAS id 1**; memory `pil_adsp_region`
  **0x53800000 + 0x2300000** (inside the UEFI "PIL Reserved" 0x4AB00000 + 0xB800000, so Windows
  does not use it).
- Interrupts: wdog SPI 0x11A; SMP2P "slave-kernel" bits fatal 0 / ready 1 / handover 2 /
  stop-ack 3; stop = bit 0 of our "master-kernel".
- `qcom,smp2p-adsp`: `qcom,smem = <443 429>` (out, in), APCS IPC bit **10**, remote pid 2.
  Out entries in the stock kernel: `master-kernel`, `rdbg`, `sleepstate` (bit 0 = apps awake).
- GLINK edge "lpass": remote pid 2, APCS IPC bit **8**, IRQ SPI 0x115. Channels: `IPCRTR`
  (intents 0x800x5, 0x2000x3, 0x4400x2), `fastrpcglink-apps-dsp`, `apr_audio_svc`
  (audio-pkt, userspace), **`adsp_apps` = GPR** (spf_core reg 3, q6prm reg 7).
- Supplies: cx = PM6125 L3 level (LPI CX), mx = PM6125 L2 level (LPI MX), both RPM rails, vote
  0x180. We do not vote them (no RPM SMD client yet) — first suspect if the ADSP never boots.
- Crash reason: SMEM global item 423.
- Firmware: `C:\topaz\fw\image\adsp.mdt` + `adsp.b00..b35` (already on the phone). 37 phdrs,
  hash in segment 1 (stored at 0x55B00000, not loaded), load range exactly
  0x53800000..0x55B00000, relocatable.
- Protection domains (`adspr.jsn`, `adsps.jsn`, `adspua.jsn`): `msm/adsp/root_pd`,
  `sensor_pd`, `audio_pd` (services tms/servreg, avs/audio), QMI instance 74.

## TopazAudio v0.1 (driver map)
Infrastructure files copied 1:1 from `drivers/TopazModem` (compat.h, port.c, log.c, fileio.c,
Smem.c, smc.asm; names like `ModemOut`/`gModemStop` kept). New:
- `Pas.c`: PAS init_image / mem_setup / segments / auth_and_reset (segments must stay inside the
  carveout), SMP2P out item 443 (`master-kernel` = 0, `sleepstate` = 1), wait READY (20 s),
  main loop, graceful stop on driver unload (stop bit → stop-ack → PAS shutdown). If item 429
  already exists (earlier driver run), the ADSP is shut down and booted again.
- `Glink.c`: multi-channel GLINK over SMEM (items 478/479/480 in partition 0↔2). Unknown
  channels the ADSP opens are acked and logged ("no client").
- `Qrtr.c`: QRTR HELLO, logs ADSP services, answers NEW_LOOKUP, pd-mapper (0x40) for the ADSP
  domains.
- `Gpr.c`: APM GET_SPF_STATE (0x01001021) every second until RSP (0x02001007) says 1.
- `driver.c`: one-shot `C:\topaz\audio.arm`, thread on core 1 (core 0 = TopazWifi's modem
  thread), log `C:\TopazAudio.log`.

## Test loop
`gh run download <run> -R MakrSas/topaz-woa -n TopazAudio-arm64` → scp to
`C:\topaz\stage\TopazAudio` → certutil Root + TrustedPublisher → `Set-Content C:\topaz\audio.arm on`
→ `devcon install TopazAudio.inf Root\TopazAudio` (first time) or `devcon update` → read
`C:\TopazAudio.log`. Risk of the first run: TZ / bus hang while starting LPASS = phone freeze →
lose SSH (ask the user before).

## Log
- 2026-10-02: v0.1 written (commit on `audio`), CI build pending.
- 2026-10-02 v0.1 on the phone: PAS init/mem_setup/auth_and_reset all 0/0 (TZ accepts the
  firmware, no hang), but the ADSP never created SMP2P item 429 in 20 s, wdog SPI never pending,
  SMEM 423 = "SFR Init: wdog or kernel error suspected." (the default text a Qualcomm DSP writes
  at start = its code ran). PAS shutdown 0/0, phone fine. Suspect: no RPM votes for LPI CX/MX
  (Linux sm6115 adsp votes proxy power domains "lcx"/"lmx" until handover). v0.2: dumps the
  0↔2 SMEM partition + 423 before boot, waits without timeout with status every 5 s.
- 2026-10-02 v0.2 on the phone: **the whole phone reset** (Kernel-Power 41, no bugcheck/dump)
  < 5 s after the second auth_and_reset. Its log shows why v0.1 "failed": before the boot the
  0↔2 partition already had items 478/479/480 (GLINK), 606, 611 and item 443 rewritten as
  "pid 2->0" = **the ADSP booted in v0.1 and reached GLINK**, we watched the wrong item.
  `qcom,smem = <443 429>` is <inbound outbound>: apps writes 429, the ADSP writes 443.
  v0.2 restarted the ADSP (PAS shutdown without stop handshake, then boot over the stale SMEM
  state) → SoC reset. **Never restart the ADSP over stale SMEM state.**
- v0.3: items fixed (out 429 / in 443); refuses to boot if item 443 or 480 already exists
  (= the ADSP ran since SMEM init → full power-off needed).
