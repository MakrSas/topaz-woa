# P9: sound (ADSP + AudioReach) — branch `audio`, driver `drivers/TopazAudio`

Goal: speaker and headphones in Windows. Roadmap item 3 (`docs/ROADMAP.md`). Work happens in the
worktree `../topaz-woa-audio` on branch `audio` (another agent works on GPU on `display`).
Rules for this work: do not touch TopazGpuW / TopazDisplay / TopazWifi; never reboot the phone; ask
the user before installing anything that could take Wi-Fi/SSH down.

## Plan
| Step | What | State |
|---|---|---|
| A1 | ADSP boot via TZ PAS + SMP2P ready | **done** (v0.3, READY+handover 58 ms after auth_and_reset) |
| A2 | GLINK lpass edge: IPCRTR (QRTR, pd-mapper), adsp_apps (GPR) | **done** (v0.3) |
| A3 | GPR: APM `GET_SPF_STATE` = 1 (AudioReach up) | **done** (v0.3, READY on the 2nd ask, 1 s) |
| A4 | Find the fitted speaker amp (I2C probe on QUP0 SE1: fs16xx 0x34 / aw87xxx 0x59 / sia81xx 0x2b) | **done**: SI-in sia81xx @0x2b (reg 0x00 = 0x60) |
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
- **2026-10-02 v0.3 WORKS** (cold SMEM after the v0.2 reset): items 429/443/480/606/611 appear
  within 40 ms, slave-kernel = 6 (READY + handover) at 58 ms. GLINK VERSION features 7 → 1.
  The ADSP opens IPCRTR, glink_ssr, adsp_apps, fastrpcglink-apps-dsp, LOOPBACK_CTL_LPASS.
  QRTR node 5; services: 0x42 servreg-notif (inst 0x4a = 74), 0x2B ssctl, 0x0F x3, 0x33 x2,
  0x18, 0x190, 0x1004 (v0x10), 0x301. pd-mapper asked only "tms/pdr_enabled" and
  "tms/pddump_disabled" (answered with 0 domains, fine). GPR: the 1st GET_SPF_STATE got
  GPR_BASIC_RSP_RESULT status 1 (not ready yet), the 2nd → **APM SPF state = 1 READY** at
  t = 1.07 s. Stable at t = 30 s (wdog 0, no FATAL). Note: the ADSP keeps running with this
  driver; reinstalling TopazAudio needs a full power-off before the next ADSP boot (v0.3 refuses).
- A4 (amp): the topaz dtbo overlay ("KHAJE IDP nopmi topaz", board-id 0x30022, read from the
  phone's dtbo_a) keeps all three amps on `qupv3_se1_i2c`, chosen at probe time: `fs16xx@34`
  (foursemi,fs16xx), `aw87xxx_pa_59@59` (awinic, aw-rx-port-id 0xb032), `sipa_i2c_L@2b`
  (si,sia81xx-i2c) + `si_pa_L` (sia81x9, owi_mode 1 = one-wire). Shared reset GPIO 106 (0x6a).
  Sound card "bengal-idp-snd-card": aux dev prefix "SpkrMonoL" = sia81xx, codecs stub + bolero +
  wcd937x, routing IN3_AUX → AUX_OUT (analog amp fed by WCD937x AUX). adsp_loader overlay names
  the firmware "adsp2" with adsp-fw-bit-values 1 (we load adsp.mdt and it works).
- v0.4: `amp.c` one-shot read-only probe (`C:\topaz\amp.probe`): no SE init (TopazBattery owns
  QUP0 SE1, no shared lock), waits for the end of a TopazBattery burst (active → 30 ms idle),
  then three register reads at DISPATCH (< 2 ms): 0x34 reg 3, 0x59 reg 0, 0x2b reg 0.
  Reset GPIO 106 is not touched yet (its TLMM tile is unknown; amps in reset may NACK).
- **v0.4 result: the amp is the SI-in sia81xx at 0x2b** (ACK, reg 0x00 = 0x60); 0x34 and 0x59
  NACK. Reset GPIO 106 was not touched, so the sia81xx answers in its current state. Window was
  found after a TopazBattery burst; TopazBattery.log shows only its normal Type-C toggling after
  the probe (no I2C errors). Unloading v0.3 stopped the ADSP cleanly (stop-ack, slave-kernel
  0x0e after 10 ms, PAS shutdown) — the next ADSP boot needs a full power-off.
  Next (A5): the sia81xx is an analog-input amp fed from WCD937x AUX_OUT (routing IN3_AUX →
  AUX_OUT), so the speaker path is: AudioReach graph → codec DMA RX → RX macro → SoundWire →
  WCD937x AUX → sia81xx (enable + mode via I2C, reg map from the SI-in sia81xx Linux driver).
- v0.5 = **lab interface**: `\\.\TopazAudio` (admin only) + `tools/audio/taudio.c` (built in CI
  into the TopazAudio artifact). Raw GPR send/recv (every GPR packet except our SPF polling goes to
  an RX ring), MMIO read/write in LPASS 0x0A000000-0x0AFFFFFF / TLMM / apps SMMU, I2C on QUP0 SE1
  in TopazBattery quiet windows, contiguous buffers. Experiments then need only a tool rebuild,
  not a driver reinstall + power-off.
- A5 facts (topaz dtbo fragment@45/48 + stock dtb): bolero v5 macros RX 0xa600000 (SoundWire
  master 0xa610000, id 2, 5 ports, hctl 0xa6a9098), TX 0xa620000, VA 0xa730000 (SoundWire master
  0xa740000, id 3, 3 ports, hctl 0xa7ec100); WCD937x RX slave (0x0a, 0x1170224) on the RX master,
  TX slave (0x0a, 0x1170223) on the VA master; WCD937x reset = TLMM GPIO 92 (msm_cdc_pinctrl@92),
  supplies L14A buck 1.8 V, L9A rxtx/vddpx 1.8 V. PRM clocks: RX core 0x30e / NPL 0x30f
  22.5792 MHz, TX 0x30c/0x30d and VA 0x307/0x308 19.2 MHz; `lpass_audio_hw_vote` = PRM HW core
  vote. LPI TLMM 0xa7c0000 (19 pins; RX SWR clk gpio3, data gpio4/5). ADSP DMA = apps SMMU SID
  0x1C1 mask 0xF (msm-audio-ion IOVA pool 0x10000000+0x10000000).
  Protocol (Linux q6prm.c): PRM module iid 2; PRM_CMD_REQUEST_HW_RSC 0x0100100F /
  RELEASE 0x01001010 with apm_cmd_header + {iid 2, PARAM_ID_RSC_HW_CORE 0x08001032, 4, 0, id}
  (1 LPASS macro, 2 DCODEC) or PARAM_ID_RSC_AUDIO_HW_CLK 0x0800102C {num 1, id, hz, attr 1, root 0}.
- Amp identified: **sia8159** (chip id reg 0x00 = 0x60 in the driver's 0x60..0x68 range; regs
  0x00-0x0A, 0x0B-0x0F NACK). Current state = off with playback defaults:
  `60 41 20 ae c9 00 28 73 88 0d a4` (SYSCTRL 0x41 = sia8159_chip_off value). Power on per the
  SI-in driver (sia8159_regs.c, e.g. Xiaomi-MT6833/kernel_xiaomi_evergo sound/soc/codecs/sia81xx):
  write playback defaults 0x01..0x0A = BD 20 AE C9 00 28 73 88 0D A4, then ALGO_CFG1 (0x05) |= 1.
  Off: 0x05 = 0, 0x01 = 0x41, 0x02 = 0x20. Lab I2C reads did not disturb TopazBattery.

### A5 lab session 1 (2026-10-02, after a cold boot: ADSP up at boot via audio.arm)
Lab scripts in `tools/audio/lab/` (dot-source `lab.ps1`; PowerShell aliases `r`/`rd` shadow
functions, hence Get-Reg/Set-Reg; one-off ssh commands can't dot-source because of the execution
policy, use `powershell -ExecutionPolicy Bypass -File`).
- **PRM works:** HW core vote DCODEC (2) → status 0 (= stock `lpass_audio_hw_vote`, ext-clk-src
  0x0b); LPASS macro (1) → status 1 (stock topaz DT doesn't use it); LPR (3) → BASIC_RSP 3
  (unsupported). Clocks 0x30e/0x30f 22.5792 MHz, 0x30c/0x30d/0x307/0x308 19.2 MHz → all 0
  (same packet as downstream audio_prm.c: attr 1, root 0).
- **LPASS registers live after the votes, no bus hang:** RX macro 0xa600400 RX0_RX_PATH_CTL = 0x04
  (reset default). RX macro MCLK_CONTROL=3, FS_CNT=1, SWR_CONTROL reset pulse → 1.
- **SoundWire masters answer:** RX 0xa610000 and VA 0xa740000 COMP_HW_VERSION = 0x01060000.
  Init as downstream swrm_master_init (+ HCTL 0xa6a9098 / 0xa7ec100 bit1 cleared, SW_RESET x2,
  BUS_CTRL 1 then 2, frame 0x101C = 0xF (50x16), auto-enum, CMD_FIFO_CFG 0x80000003, COMP_CFG 3).
  COMP_STATUS bit0 (frame gen) = 1, but: IRQ bit3 (bus clash) always set, MCP_SLV_STATUS 0,
  no enumerated device on either master; SoundWire read cmds come back "ignored" (irq bit31).
- Pins: LPI gpio0..5 = func1 (0xD04 clk / 0xD06 data, 10 mA), slew 0x3F3F at 0xa95a000;
  TLMM gpio92 (WCD937x reset, WEST, 0x55c000) out high (CTL 0x200, IO 2). The LPI IN bit of the
  clock pins stays 0 in every sample (data pins do follow the pad) → **the SWR clock does not reach
  the pad**; the codec never sees a bus.
- Also done without effect: bolero fs-gen sequence (VA MCLK, FS cnt, TOP_CFG0 bit1 broadcast),
  RX mclk muxsel 0xa5640d8 = 1 (downstream mux1 path since rx default-clk-id = TX), NPL clocks
  for TX/VA, reset released before/after bus start.
- L9 (WCD937x rxtx/px 1.8 V) is on (TopazWifi pmic log: 80/87/0708); L14 (buck) not checked.
- Open questions: what gates the SWR clock output (a clock the stock kernel gets from some node
  we haven't mapped? the failing LPASS macro vote?), L14 state. Next: dump the whole RX macro
  CLK_RST block + VA/TX TOP CSR registers, compare with reset defaults from lpass-rx-macro.c /
  downstream bolero register defaults; try the TX macro SWR path; read L14 via SPMI (needs an
  allowlist entry → driver update + power-off).
- **2026-10-02 17:39 and 17:47: two watchdog reboots caused by lab reads** of 0x0A7E0100 and of
  whole 0x0A6A9000 / 0x0A7A0000 pages (bus hang). v0.5.2 restricts lab MMIO to known blocks.
  Also learned: a normal Windows restart (`shutdown /r`) leaves SMEM clean, the ADSP boots again
  (no full power-off needed). On bengal the VA SoundWire master is clocked by the VA macro SWR
  gate (0xa730008), not by the TX macro as on agatti; VA/TX CGCR = 0xa7ec100 (agatti lpasscc @
  0xa7ec000). `tools/audio/lab/bringup.ps1` = reproducible bring-up from a fresh boot.
- v0.5.3: GLINK remote intent table 64 → 256 (APM GRAPH_OPEN 296 B was stuck in txq). A one-module
  CODEC_DMA_SINK graph on RX_CODEC_DMA_RX_0 opens/prepares/starts (status 0); SoundWire unchanged.
- v0.5.4: lab PMIC read. **L14 (WCD937x vdd-buck) is OFF** (en 00, status 03, 1.80 V set), L9 on.
  Next: RPM SMD client to vote L14 on (see HANDOFF_p9_audio.md).

### 2026-10-04: L14 on via RPM (TopazRpm v0.8, branch display)
- TopazRpm v0.8 + C:\topaz\rpm.l14: RPM acked "ldoa" id 14 uv=1800000 + swen=1 (active + sleep set).
  PMIC confirms: 0x4d46 = 80 (enabled), 0x4d08 = 87, vset 1.80 V. ADSP READY, APM ready as before.
- **Codec still does not attach.** bringup.ps1 after L14: same as before. Both masters now inited
  (VA master needs VA macro SWR_CONTROL 0xa730008 = 2,3,1 first - bringup.ps1 did not do it, VA
  master read all zeros / SW_RESET_STATUS 1 until then): COMP_STATUS 0x2a01, IRQ 0x2008 = MASTER_CLASH_DET
  + BUS_RESET_FINISHED, MCP_SLV_STATUS 0, no enumerated id.
- Pads: clock pins (gpio0 / gpio3, func1 0xD04) read IN = 0 always, data pins (gpio1 / gpio4, func1
  0xD06 bus-hold) read 1 always. gpio4 as plain GPIO input: pull-down -> 0, pull-up -> 1, no pull -> 1
  (keeper): nobody drives the data line. So on BOTH buses the master drives neither clock nor data
  onto the pads while it reports link active - the clash is the keeper's 1 vs the master's 0.
  Common factor of both buses: the TX core clock (RX default-clk-id = TX, VA uses TX_MCLK) and the
  LPI pad function routing. RX mclk muxsel 0xa5640d8 reads 0 (downstream sets 1 after enabling RX
  core clock when dev_up_gfmux).
- Next ideas: prove whether MCLK really runs in the macros (a register that only latches with MCLK,
  or FS counter); check LPI cfg bits 10/11 (0xC00, not set by downstream's 0x104 = func1 + 10 mA);
  VA chip-wakeup reg 0x3ca04c (DT va_swr_clk_data_pinctrl qcom,chip-wakeup-reg, default 1, bit 0).
- Pad experiments (same boot, 2026-10-04): SWR clock pads gpio0 / gpio3 in func1 simply follow the
  pull (pull-up -> 1, pull-down -> 0): **the master never drives its clock pad** (tri-stated), on
  both buses. Data pads with pull-down read 0 with a rare 1. MASTER_CLASH (irq bit 3) comes back
  right after a clear even with the codec held in reset (gpio92 low) -> self clash, not the codec.
  DMIC check: VA DMIC0_CTL 0xa730084 = 1 and gpio6 (dmic01_clk) func1 -> pad driven, but stuck low
  (pull-up still reads 0), no toggling. Function routing to the pads works (gpio6 is driven), so the
  likely common cause is **no MCLK in the macros** although PRM acks the 6 codec clocks (same packet
  as spf audio_prm.c: attr COUPLE_NO, root 0, DT freqs) and the macro registers are writable.
  RX muxsel 0xa5640d8 = 1 written (reads back 1) - no change. 0x3ca04c (VA chip-wakeup reg) is
  outside the lab allowlist.
- Next: find what else gates the codec MCLK on khaje SPF (LPASS core "macro" vote id 1 fails with
  status 1 - maybe it needs a client handle / different payload; compare the PRM HW_CORE packet with
  spf audio_prm.c prm_cmd_request_hw_core_t field by field), and look for an LPASS clock status
  register we can read safely.
- TopazRpm v0.9 + C:\topaz\rpm.lpi: LPI MX/CX (rwlm/rwlc vlvl 0x180, the ADSP's cx/mx supplies in DT)
  acked - no change: both masters status 0xa01, irq 0x2008, clock pads still undriven (pull-up reads 1).
  Ruled out now: L14, LPI rails, LPI cfg bits 10/11, RX muxsel, a running CODEC_DMA graph (started,
  status OK) - pads stay undriven. tools/audio/lab/padtest.ps1 = the pull-up pad check; bringup.ps1
  now also starts the VA macro SWR clock.
- Mainline Linux has sm6115 lpass macro drivers (lpass-rx/tx/va-macro, soundwire qcom.c v1.6) but no
  DT for a board with it; mainline swrm init = ours (CGCR reset pulse, BUS_CTRL CLK_START 2, ...).
- Proposed: get a ground-truth register dump from the stock Android (ColorOS vendor kernel) while a
  sound plays: bolero regmap debugfs, swrm reg dump, LPI pin cfg - and diff with ours.

### 2026-10-04: WCD937x ATTACHED (stock Android ground truth)
- Booted the stock Android (slot a, HyperOS GSI + ColorOS vendor kernel) with Magisk in init_boot_a
  (`fastboot --set-active=a`; init_boot_a = Magisk-patched backup_20261001 copy, Windows slot b
  untouched), dumped debugfs (bolero regmap, rx/va swrm_reg_dump, LPI pinmux, wcd937x regmap) and an
  ftrace (regmap writes + the drivers' trace_printk) of a cold playback start:
  `docs/logs/p9_stock/`.
- **Root cause:** stock requests only the TX core + TX NPL clocks (0x30c / 0x30d, 19.2 MHz); RX and VA
  run on the TX MCLK (default-clk-id 0, muxsel left 0). Our bring-up also requested RX 0x30e/0x30f
  (22.5792 MHz) and VA 0x307/0x308 - with those the macros had no working MCLK and the SoundWire
  masters never drove their pads. Also aligned with stock: MCP_CFG read-modify-write (reset 0x3fff00,
  we zeroed bits 8..15) and frame bank 0 = 0x2f0008.
- **Result (bringup.ps1, fresh ADSP boot):** RX master MCP_SLV_STATUS 4, enumerated 0x01170224 (wcd937x
  rx slave), VA master 0x01170223 (tx slave); clock pads toggle. Next: WCD937x init (stock regmap
  writes in playback_start_trace.txt), AUX path + sia8159, data path.

### 2026-10-04: FIRST SOUND from the speaker (Windows, lab scripts)
User: tone at full volume. Chain: shared-memory sine (taudio buf) -> SH_MEM_PULL_MODE -> CODEC_DMA_SINK
(RX_CODEC_DMA_RX_1 = AIF2_PB -> RX_MACRO RX2 -> INT2 -> AUX) -> RX SoundWire ports 2 (CLSH) / 4 (LO) ->
WCD937x AUX PA -> sia8159. Sequence after a fresh ADSP boot (tools/audio/lab, all on the phone in C:\topaz\stage):
1. `bringup.ps1` - DCODEC vote, TX core/NPL clocks only, pins, macros, both SoundWire masters, codec reset.
2. `smmu_audio.ps1` - identity context bank for the audio DMA stream 0x1C1 (DT msm-audio-ion).
3. `rx_stock.ps1` - 119 RX macro registers from the stock dump (path CTL last).
4. `wcd_aux.ps1` - WCD937x init_reg + RX clocks + AUX DAC (TX slave on the VA bus, dev 1).
5. `tone.ps1` - buffers + APM_CMD_SHARED_MEM_MAP_REGIONS (raw `taudio cmd`, no apm_cmd_header; msw 1 =
   SID offset; pos buffer with property 0x2 is required or PULL_PUSH_MODE_CFG fails with status 1),
   graph open/config/prepare/start. The pos buffer index advances = the DSP consumes the buffer.
6. `ports.ps1` (bank 1 port config + broadcast bank switch), `wcd_auxpa.ps1`, `amp.ps1`, RX2 unmute 0x24
   (`play.ps1` runs 5-6). Stop: `graphstop.ps1`, amp off: `taudio i2c 0x2b 5 0`.
Too loud: amplitude 0.3 + stock amp gain. Next: volume (RX2 digital gain 0xa600510? / sia8159 gain,
lower sine amplitude), then a real Windows audio endpoint (ACX / portcls) on top of this chain.
- Volume: RX2 digital gain = `RX_RX2_RX_VOL_CTL` 0xa600514 (s8 dB, -84..+40, stock control "RX_RX2 Digital
  Volume"), `vol.ps1 -Db N`. `beep.ps1` (codec already up): -30 dB + amplitude 0.1 = comfortable (user).
