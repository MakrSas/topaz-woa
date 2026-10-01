# Handoff: Wi-Fi on topaz, from UEFI spike to a Windows driver (2026-10-01)

> **Update 2026-10-01: section 2 is DONE.** `drivers/TopazModem` v0.3 reaches `WLAN FIRMWARE READY`
> in Windows and keeps the modem up (log `docs/logs/TopazModem-v0.3.log`, details in
> `RESEARCH_wifi.md` P2b). Deploy: `tools/deploy/install-modem.cmd` (copies `fw\` to `C:\topaz\fw`,
> creates the one-shot `C:\topaz\modem.arm`, installs). Next is P4 (section 5).

Read this together with `docs/RESEARCH_wifi.md` (facts and protocol details) and
`docs/AGENT_BRIEF_drivers.md` (hardware, build/deploy rules).

## 1. Where we are

Everything below works **in UEFI** (`uefi/TopazOtgDxe`, boot menu item "Modem test"),
tested on the phone with `fastboot boot` (RAM), nothing flashed:

| Step | File | Result on the phone |
|---|---|---|
| P1 modem boot via TrustZone PAS | `ModemPas.c` | init_image / mem_setup / auth_and_reset = 0/0 |
| SMP2P handshake | `ModemPas.c` | apps creates item 428 -> modem `slave-kernel` = READY + handover in 0.25 s |
| SMEM v12 | `Smem.c` | partitions, item get/alloc under TCSR hwlock 3 |
| GLINK over SMEM | `Glink.c` | channel IPCRTR open both ways, intents, RX_INTENT_REQ |
| QRTR | `Glink.c` | HELLO, NEW_SERVER, flow control (RESUME_TX) |
| pd-mapper / tftp / rmtfs | `ModemSvc.c` | modem loads wlanmdsp.mbn (3.8 MB), EFS read, ~43 services |
| WLFW QMI (ath10k order) | `Wlfw.c` | **FW_READY_IND** received, modem stays up |

Commits on branch `display`: `200d9b4` (P1), `fd6b829` (P2), `46cced6` (P3).

## 2. Goal of the next session: `drivers/TopazModem` (KMDF, Windows)

Same flow as UEFI, but inside Windows, so the modem and the WLAN firmware stay served
while Windows runs. Success = `C:\TopazModem.log` shows `WLAN FIRMWARE READY` and the
modem does not go FATAL for minutes.

Not in scope yet: the WLAN data path (copy engines at 0xC800000, HTT/WMI, SMMU stream
0x1A0) and a WiFiCx miniport — that is P4, weeks of work.

### Architecture decisions (already made, keep them)

- **The Windows driver boots the modem itself** (PAS via SMC). Do *not* run "Modem test" in
  UEFI before booting Windows: GLINK/QRTR state lives in UEFI RAM and is lost, and the modem
  would sit 30-60 s without rmtfs/tftp. The driver must check SMEM item 435 first and
  refuse (log it) if the modem is already running -> cold boot needed.
- Root-enumerated KMDF driver like `drivers/TopazBattery` (Class System), one system thread
  that polls (no interrupts), `ExSetTimerResolution` to 1 ms; when idle sleep 1 ms with
  `KeDelayExecutionThread`, when busy loop with a short `KeStallExecutionProcessor`.
  Don't spin a core at 100 %.
- Log to `\??\C:\TopazModem.log` (copy `drivers/TopazBattery/log.c`).
- Port the UEFI files with a thin compat layer so they stay nearly identical.
  A first `drivers/TopazModem/compat.h` exists (types, CopyMem/ZeroMem/CompareMem,
  AllocatePool family, MmioRead32/Write32, string macros, a tiny `gBS`, an
  `EFI_FILE_PROTOCOL` emulation, `ARM_SMC_ARGS`). Review it, finish it.

### Porting notes (things that differ from UEFI)

1. **No identity mapping.** UEFI code casts physical addresses to pointers. In Windows:
   - SMEM 0x46000000, 2 MiB: `MmMapIoSpaceEx(..., PAGE_READWRITE | PAGE_WRITECOMBINE)`
     (Normal non-cacheable; Device memory would fault on unaligned memcpy). Make
     `SMEM_BASE` a variable VA in the Windows `Modem.h`.
   - APCS IPC 0x0F111000 (+8), TCSR mutex 0x340000 (+0x1000 * 3): `PAGE_NOCACHE`.
   - Modem carveout (ELF segments, 0x4AB00000..0x51400000, "PIL Reserved" in the UEFI map,
     so Windows does not use it): map WC only while loading segments, unmap after
     auth_and_reset (TZ locks it).
   - Buffers TZ or the modem read by physical address (PAS metadata, SCM assign argument
     blocks, the 3 MiB rmtfs buffer): `MmAllocateContiguousMemorySpecifyCache` below 4 GiB,
     `MmNonCached`, then `MmGetPhysicalAddress`. rmtfs iovec entries carry physical
     addresses -> translate to the VA of that buffer.
   - MSA region 0x51900000 (1 MiB) is only passed by address + SCM assign, never mapped.
2. **SMC:** MSVC has no inline asm on ARM64. Add `smc.asm` (armasm64, MARMASM item in the
   vcxproj with `marmasm.props/targets`): load x0..x7 from `ARM_SMC_ARGS`, `smc #0`,
   store x0..x7 back. Keep the struct pointer on the stack (x0-x17 may be clobbered).
   Long calls return 1 (INTERRUPTED): resume with x0 = 1, same x1..x5, x6 from the result
   (as in `ModemPas.c` `Scm()`). Windows runs at EL1 under the same Qualcomm hypervisor as
   UEFI, so SiP calls should behave the same — verify with the "call available" log lines.
3. **Printing:** UEFI `Out()` uses EDK2 formats: `%a` = char*, `%s` = CHAR16*, `%lu/%lx`
   = 64-bit, `%r` = EFI_STATUS. MSVC: `%s`, `%ws`, `%llu/%llx`. Either translate the format
   string at runtime in `ModemOut()` / `AsciiSPrint()` or edit the call sites.
4. **Files:** `EFI_FILE_PROTOCOL` (Open/Read/SetPosition/GetInfo/Close) over
   `ZwCreateFile/ZwReadFile/ZwQueryInformationFile`, root = `\??\C:\topaz\fw`, so
   `\image\modem.mdt` -> `C:\topaz\fw\image\modem.mdt`.
5. **rmtfs:** UEFI reads the real partitions through BlockIo. In Windows read the backups as
   files: `C:\topaz\fw\efs\modemst1.bin`, `modemst2.bin`, `fsc.bin`, `fsg.bin`.
   Writes stay dropped (as in UEFI) until we decide to persist them.
6. **Time:** `ModemMs()` -> `KeQueryInterruptTime()` (100 ns units).
7. The GLINK main loop in `Glink.c` runs for N seconds; in Windows run until driver stop or
   modem FATAL, print a one-line status every ~30 s, and on FATAL log SMEM item 421.

### Gotchas already paid for (do not rediscover)

- RESUME_TX must be the full 20-byte `qrtr_ctrl_pkt`; with 12 bytes the modem ignores it and
  blocks after 10 packets to one port -> tftp stalls at 100 blocks -> "User-PD grace timer
  expired for wlan_process".
- The modem's QRTR packets set confirm_rx; answer every one with RESUME_TX.
- Remote intent table must be large (256); intents granted on request are reused.
- tftp: a request with `rsize 0` + `tsize` is a stat (answer OACK with tsize, the modem ends
  it with ERROR 9 "End of Transfer"); the real read comes with rsize = file size,
  blksize 7680, wsize 10. Each session answers from its own port.
- SMEM item 421 at boot reads "SFR Init: wdog or kernel error suspected." — placeholder,
  not a crash.
- Slow logging makes the WLAN PD miss its ~38 s grace timer: keep the hot path quiet.

## 3. Files to stage on the phone (`C:\topaz\fw`)

- `image\` = the **whole** content of the NON-HLOS FAT from `modem_a`, including
  subdirectories (`image\modem_pr\mcfg\configs\...` is requested by the modem; the old
  extraction in `~/work/wifi/fw/image/` on the laptop has no subdirectories — re-extract):
  `ssh s8build`, `gunzip -k ~/work/topaz/backup/modem_a.img.gz`, then `7z x` or
  `mtools`/`mcopy -s` the image (FAT16, 4 KiB sectors).
  Must contain modem.mdt, modem.b00..b29, wlanmdsp.mbn (crc32 a4fd8c71), bdwlan.*,
  modem_pr\mcfg\configs\mbn_hw.dig / mbn_sw.dig.
- `efs\modemst1.bin`, `efs\modemst2.bin`, `efs\fsc.bin`, `efs\fsg.bin` from
  `~/work/topaz/backup/{modemst1,modemst2,fsc,fsg}.img.gz` (gunzip).
- Copy via the user's exFAT flash drive (`/Volumes/Образы/topaz-woa/TopazModem/fw/...`) and an
  `install.cmd` step that does `xcopy /e /i` into `C:\topaz\fw`.

## 4. Build / deploy / test loop

- Add `drivers/TopazModem` to `.github/workflows/build.yml` (build step, package loop,
  artifact `TopazModem-arm64`), like the other drivers. Push to `display`, wait for CI,
  `gh run download <id> -n TopazModem-arm64`.
- Add `tools/deploy/install-manual.cmd` as `install.cmd`, copy to the flash drive, the user runs
  it in Windows, reboots if needed, and brings back `C:\TopazModem.log`.
- Expected log milestones, in order: SCM "call available" -> PAS 0/0 -> SMP2P READY ->
  GLINK IPCRTR open -> QRTR HELLO -> pd-mapper requests -> rmtfs open/alloc ->
  tftp wlanmdsp.mbn done -> WLFW 0x45 up -> ... -> `WLAN FIRMWARE READY`.
- Risks to check first: SMC from the Windows kernel actually reaching TZ; whether
  `MmMapIoSpaceEx` accepts the SMEM/carveout ranges; contiguous allocation below 4 GiB.

## 5. After that (P4, weeks)

ath10k SNOC: copy engines at 0xC800000 (12 CE IRQs SPI 0x166..0x171), WLAN_CFG (CE/service
pipe config, shadow regs) + WLAN_MODE mission, HTT/WMI over CE, DMA through apps SMMU
stream 0x1A0 (check whether it is bypass or translating), then a WiFiCx miniport.
Reference: Linux `drivers/net/wireless/ath/ath10k/{snoc,ce,htt*,wmi-tlv}.c`.
