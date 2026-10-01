# Handoff: state on 2026-10-01 late + next work (GPU)

Read this first in a new session. Branch `display`. User language: Russian (often types Russian in
the English layout — decode it). Estimates in hours/days (Wi-Fi took one day end to end).

## Working with the phone
- **SSH**: `ssh topaz-win` (Mac `~/.ssh/config`: makr@192.168.0.105, key `~/.ssh/topaz_win`). Windows
  PowerShell 5.1, elevated. Prefix commands with `[Console]::OutputEncoding=[Text.Encoding]::UTF8;`.
  scp works (`scp f topaz-win:C:/topaz/...`). For complex PowerShell, write a .ps1 locally, scp it,
  run with `powershell -NoProfile -ExecutionPolicy Bypass -File`. Installed with
  `tools/deploy/enable-ssh.cmd` (Win32-OpenSSH 10.0 ARM64 zip; Windows Update FoD gave 0x80240023).
- SSH rides on our TopazWifi: a broken Wi-Fi build = no SSH → flash drive fallback.
- **Ask before installing drivers or rebooting** — the user uses the phone at the same time.
- Driver install over SSH: scp the CI artifact to `C:\topaz\stage\<Name>`, then
  `certutil -addstore -f Root topaz-woa-test.cer; certutil -addstore -f TrustedPublisher ...;
  .\devcon.exe install <Name>.inf Root\<Name>` (root-enumerated, no reboot needed).
- The phone's clock is correct now (2026), internet via our Wi-Fi.

## Done in this session
- `docs/ROADMAP.md`: full roadmap (estimates recalibrated to hours/days on the user's request).
- **TopazCpu v0.1** (`drivers/TopazCpu`): the bootloader left the gold cluster at **300 MHz**;
  with `C:\topaz\cpu.max` both clusters run at the top level (silver 1900, gold 2803 MHz). Bench
  (`tools/cpu-bench.ps1`, single-core SHA-256): gold 2 → 21 MB/s. User: "стало СИЛЬНО лучше".
  One unexplained hang right after (user killed a task → black screen → hard reboot); stable after.
  CURRENT_VOTE 0x704 is not the running level on this SoC (constant 0x0474009e) — remove in v0.2.
  Next for CPU: load-based governor, thermal watch (battery NTC from TopazBattery).
- VPN: Happ's TUN mode failed (its sing-box adapter "happ-tun" never came up; standalone xray and
  sing-box TUN work, Wintun 0.14 installed). User switched to FlClash — works. FlClash's blur
  makes the screen ~2 fps while its window is open (no GPU) — the trigger for the GPU work.

## Now: GPU (Adreno 610) — `docs/P8_gpu.md`
- Path 1 (Qualcomm 7c driver) rejected: KMD requires a GMU, A610 has none.
- Path 2: own driver = G1 KMDF bring-up test (`TopazGpu`) → G2 WDDM render-only KMD → G3 Mesa
  d3d10umd + freedreno + WDDM winsys. All facts, registers, firmware locations and the G1 step list
  are in P8_gpu.md.
- Artifacts on s8build `~/work/gpu/`: `a630_sqe.fw`, `qcdx7180.cab` + `qcpep.wd7180.cab`
  (proprietary, reference only), `lp.py` (super.img LP parser), `vendor_a.img` (ColorOS vendor,
  691 MB) → extract `a610_zap.*` from it (`fsck.erofs --extract` or debugfs, depending on the fs).
- User gave blanket permission to download what is needed for this work ("качай все что можно").
- **Firmware extracted** to s8build `~/work/gpu/vfw/` (a610_zap.mdt/.b00-.b02/.elf, vendor a630_sqe.fw).
- **State 2026-10-02 (TopazGpu v0.5 installed on the phone, commit 7ceabdc):**
  - v0.1: the bootloader leaves the GPU fully off. v0.3: **GPU powered and responding** (CX/GX GDSC
    PWR_ON, core 300 MHz from GPLL0, CP_ALWAYS_ON_COUNTER ticks 19.2 MHz). v0.4: GPU SMMU = 4 SMRs /
    5 CBs, all unconfigured. v0.5: **zap shader accepted by TZ** (PAS 13, mem_setup size must be
    4K-aligned). Details + register values: P8_gpu.md "Results".
  - Test loop: `gh run download <run> -n TopazGpu-arm64` → scp to `C:\topaz\stage\TopazGpu` →
    `Set-Content C:\topaz\gpu.on on` (one-shot flag, the driver deletes it) → `devcon update
    TopazGpu.inf Root\TopazGpu` (or `devcon restart Root\TopazGpu`) → read `C:\TopazGpu.log` (last
    `==== TopazGpu` block). The user allows installing without asking (2026-10-02).
  - **Next = v0.6 (CP start), see P8_gpu.md "G1 v0.6 plan"**: identity SMMU bank for SID 0 mask 1,
    SQE (vendor a630_sqe.fw minus its first dword) into a buffer below 4 GB, 32 KB ring, hw_init
    subset, SQE_CNTL=1, CP_ME_INIT, CP_SET_SECURE_MODE 0, CP_MEM_WRITE magic → read back.

## Still open (older)
- Wi-Fi P6 HT/VHT (`docs/HANDOFF_p6_htvht.md`), then P7 (CE interrupts, real MAC, stats).
- Archive TopazWifi v0.3/v0.4 logs (redact SSIDs).
- Rules: never flash partitions / touch GPT; never ntfsfix the Windows partition; one agent drives
  the phone; redact SSIDs in committed logs; .cmd files CRLF; never mention a UEFI "Modem test".
