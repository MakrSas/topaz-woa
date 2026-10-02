# Handoff: GPU (Adreno 610) work — state on 2026-10-02

Read this first in a new session, then `docs/P8_gpu.md` (all facts, register values, logs, plans).
Repo `MakrSas/topaz-woa`, branch `display`. User language: Russian (often types Russian in the
English keyboard layout — decode it). Estimate in hours/days (Wi-Fi took one day end to end); the
user asks to **write everything down after every step** (they may switch Claude accounts).

## Working with the phone
- **SSH**: `ssh topaz-win` (Mac `~/.ssh/config`: makr@192.168.0.105, key `~/.ssh/topaz_win`). Windows
  PowerShell 5.1, elevated. Prefix commands with `[Console]::OutputEncoding=[Text.Encoding]::UTF8;`.
  scp works (`scp f topaz-win:C:/topaz/...`). For complex PowerShell write a .ps1 locally, scp it,
  run with `powershell -NoProfile -ExecutionPolicy Bypass -File`. Installed with
  `tools/deploy/enable-ssh.cmd` (Win32-OpenSSH 10.0 ARM64 zip; Windows Update FoD gave 0x80240023).
- SSH rides on our TopazWifi: a broken Wi-Fi build = no SSH → flash drive fallback.
- **The user allows installing/updating our test drivers without asking** ("больше не спрашивай,
  просто ставь", 2026-10-02). Still warn before a reboot — they use the phone at the same time.
- Driver install: `gh run download <run> -R MakrSas/topaz-woa -n <Name>-arm64` → scp to
  `C:\topaz\stage\<Name>` → `certutil -addstore -f Root topaz-woa-test.cer; certutil -addstore -f
  TrustedPublisher topaz-woa-test.cer; .\devcon.exe install|update <Name>.inf Root\<Name>`
  (root-enumerated, no reboot). CI: push to `display` → workflow "Build drivers".

## Phone state right now
- TopazWifi (Wi-Fi + modem), TopazBattery, TopazButtons, TopazTouch, TopazDisplay as before.
- **TopazCpu v0.1** installed + `C:\topaz\cpu.max` present → both CPU clusters at the top level
  (silver 1900, gold 2803 MHz; the bootloader leaves gold at 300 MHz). Undo: delete cpu.max +
  `devcon restart Root\TopazCpu`. Next for CPU: load-based governor + thermal watch; drop the
  meaningless CURRENT_VOTE 0x704 logging.
- **TopazGpu v0.6** installed. `C:\topaz\gpu.on` was consumed, so at boot it only logs the clock tree
  (GPU stays off). GPU firmware on the phone: `C:\topaz\fw\gpu\` (a610_zap.mdt/.b00-.b02/.elf,
  vendor a630_sqe.fw); also on s8build `~/work/gpu/vfw/`.
- VPN: user uses FlClash (works); Happ's TUN mode was broken in Happ itself. FlClash's blur window
  drops the UI to ~2 fps — no GPU yet.

## GPU: where we are
- Path 1 (Qualcomm 7c `qcdx7180` driver) rejected: its KMD is built around a GMU, A610 has none.
- Path 2 (chosen): G1 KMDF bring-up (`drivers/TopazGpu`) → G2 WDDM render-only KMD → G3 Mesa
  d3d10umd + freedreno + WDDM winsys.
- **G1 DONE (TopazGpu v0.6, ce2b611): the GPU executes commands under Windows** — power-up, zap via
  TZ PAS 13, identity SMMU, SQE microcode, ring, CP_ME_INIT, CP_SET_SECURE_MODE 0, CP_MEM_WRITE
  0xC0FFEE00 read back by the CPU, no SMMU fault. Log excerpt in P8_gpu.md.
- Test loop for TopazGpu: push → CI → download/scp → `Set-Content C:\topaz\gpu.on on` (one-shot,
  the driver deletes it before trying) → `devcon update TopazGpu.inf Root\TopazGpu` → read the last
  `==== TopazGpu` block of `C:\TopazGpu.log`. Bump `TOPAZ_GPU_VERSION` each build.

### TopazGpu driver.c map
`DumpClocks` (GCC/GPU CC register table) · `PowerUp`/`PowerDown` (GDSC + clocks) · `FirstGpuReads` ·
`Scm` + `smc.asm` (TZ calls with INTERRUPTED resume) · `ReadWholeFile` · `ZapLoad` (PAS 13) ·
`SmmuDump` · `SmmuIdentity` (CB0 pass-through, SMR0 SID 0 mask 1) · `HwInitRegs` (Linux hw_init
subset) · `Pkt7`/`Emit`/`Submit` (ring, waits RPTR == WPTR, logs SMMU fault on timeout) · `CpStart`
/`CpStop`/`CpFree` · `HwStart`/`HwStop` (PrepareHardware/ReleaseHardware).

### Gotchas learned
- Never read GPU (0x5900000) or GPU SMMU registers with the GDSCs/clocks off → bus hang.
- CX GDSC must be on **before** the GPU CC cxo/ahb branches report running (v0.2 aborted).
- Zap: metadata = the whole .mdt; PAS mem_setup size must be **4 KB-aligned** (0x1000, not 0x830),
  else 0xffcfffba / auth 0xffcfffbc. Do PAS shutdown(13) before re-init.
- SQE: drop the first dword of a630_sqe.fw. Keep GPU buffers below 4 GB, contiguous, MmNonCached.
- GX_GFX3D can run from GCC GPLL0 (parent 5, /2 = 300 MHz) — the GPU CC PLLs are not configured yet.

### Next steps (detail in P8_gpu.md "Next (G1.5 → G2)")
1. G1.5: GPU IRQ (SPI 177 → GSIV 209 needs an ACPI resource or a direct GIC hookup; or poll for now),
   CP_EVENT_WRITE/CACHE_FLUSH_TS fences, a real draw/blit (2D clear) into a buffer and copy it to the
   framebuffer 0x5C000000 to show it; HWCG/CP protect/UBWC from Linux; GPU CC PLL for higher clocks.
2. G2: WDDM render-only KMD (TopazDisplay stays display-only).
3. G3: Mesa (d3d10umd + freedreno + new WDDM winsys) built for Windows ARM64 in CI.

## Step B status (2026-10-02 evening): DONE — see "STEP B DONE" at the end of P8_gpu.md
D3D11 device + HLSL VS/PS draws render correctly on the Adreno (d3dtest tri/clear/copy). Phone:
TopazGpuW v0.25 (wedge recovery without PnP restart), UMD in System32 (opt-in TOPAZGPU_ENABLE).
Test: `C:\topaz\t.ps1 <tri|tricull|clear|copy|vsonly|nort> [FD_MESA_DEBUG]` (tools/gputest/t.ps1).
Debug: FD_RD_DUMP=enable|full → C:\tmp\*.rd → cffdump on s8build (~/work/gpu/mesa-tools/...).
Rules learned: never `devcon restart/update` TopazGpuW while DWM holds it (PnP lock → explorer,
network, sshd hang); install a new KMD via guard file + normal reboot (see P8 notes).


## Side work 2026-10-02 evening: brightness (see docs/NOTES_brightness.md)
TopazBacklight (DCS 0x51 over DSI0, DMA path) works via C:\topaz\brightness; TopazDisplay v0.3 with
the Windows brightness interface is built but not installed. GPU step C state: ACPI GPU0 in
`Mu-topaz-v4-GPU0-RELEASE.img` (RAM boot only), the ACPI adapter needs full display (C2) — see the
end of P8_gpu.md; phone currently booted from the flashed UEFI (no GPU0, no TopazGpuW instance).

## Still open (older)
- Wi-Fi P6 HT/VHT (`docs/HANDOFF_p6_htvht.md`), then P7 (CE interrupts, real MAC, stats).
- Archive TopazWifi v0.3/v0.4 logs (redact SSIDs). Roadmap: `docs/ROADMAP.md`.
- Rules: never flash partitions / touch GPT; never ntfsfix the Windows partition; one agent drives
  the phone at a time; redact SSIDs in committed logs; .cmd files CRLF; never mention a UEFI
  "Modem test" item to the user.
