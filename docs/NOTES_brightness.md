# Panel brightness under Windows — SUMMARY (2026-10-02, DONE: the Windows slider controls the panel)

How it works now:
- **TopazDisplay** (`drivers/TopazDisplay`, root KMDOD `Root\TopazDisplay`) owns the panel; **Microsoft Basic Display is
  disabled** (`ConfigFlags=1` on `HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\BASICDISPLAY\0000`, applied at boot — a hot
  disable of an adapter DWM holds can hang PnP; leaving both enabled = two monitors on one framebuffer, artifacts).
- DxgKrnl asks TopazDisplay for `GUID_DEVINTERFACE_BRIGHTNESS`; `SetBrightness(0..100)` sends DCS 0x51 (level
  1..0x7FF) through DSI0 `0x5E94000` with the DMA command path (`topaz_bl.cxx`).
- The slider only exists because the monitor `DISPLAY\TPZ6225` has `BrightnessControl=1` (REG_DWORD) in its driver key
  `Class\{4d36e96e-e325-11ce-bfc1-08002be10318}\<n>`: **`tools/deploy/topaz-brightness.ps1`** sets it (needs a reboot).
  An INTERNAL/LVDS child type is impossible (dxgkrnl rejects it for a non-POST root adapter), the child type is OTHER.
- TopazBacklight is disabled (`devcon disable Root\TopazBacklight`) so only one driver writes DSI0; v0.4 makes the
  DMA path its default (`C:\topaz\dsi.fifo` forces the old FIFO path) in case it is used as a fallback.
- `C:\topaz\td.cfg` = `-1 4` is on the phone (child technology/HPD override, read at each QueryChildRelations); code
  default is now OTHER too (v0.7), so the file is no longer needed.
- Phone state at the end: TopazDisplay **v0.6** running (v0.7 built by CI but NOT installed — never `devcon
  update/restart` the display adapter while DWM holds it; install via normal reboot), BasicDisplay disabled,
  TopazBacklight disabled, `BrightnessControl=1` set, boot ETW autologger `Autologger\TopazDxg` → `C:\topaz\boot.etl.001`
  (remove with `reg delete HKLM\SYSTEM\CurrentControlSet\Control\WMI\Autologger\TopazDxg /f` when no longer needed).
- Recovery if the screen stays on the boot logo: over SSH `reg add ...ROOT\BASICDISPLAY\0000 /v ConfigFlags /t
  REG_DWORD /d 0 /f; devcon enable "@ROOT\BASICDISPLAY\0000"` (works without a reboot), then fix TopazDisplay.
- Tools: DxgKrnl ETW recipe + manifest event names in the section "TopazDisplay v0.3 on the phone"; the research report
  section explains the dxgkrnl/monitor.sys logic (PDB-based disassembly lives on s8build `~/work/dxgre`).
- Verified by the user: screen off/on with the power button keeps the brightness level (no resume handling needed
  for that case). Not tested: lock-screen timeout, sleep/Modern Standby.
- Open (not needed now): brightness curve (linear % -> DBV today: 1 % = 0x014, 50 % = 0x3FF; an AMOLED-friendly
  table/gamma would feel more even), retry on "NO DONE", no ambient light sensor (needs ADSP) so no auto-brightness.
- Remaining nice-to-have: monitor INF for `MONITOR\TPZ6225` with `HKR,,BrightnessControl,0x00010001,1` instead of the
  script; persist/restore the brightness level across boots is done by Windows itself (it calls SetBrightness).

---

# Panel brightness under Windows (TopazBacklight) — running notes

Started 2026-10-02 evening. Driver: `drivers/TopazBacklight` (root KMDF, `Root\TopazBacklight`),
log `C:\TopazBacklight.log`, staged on the phone in `C:\topaz\stage\TopazBacklight`.

## Panel facts (stock DT, s8build `~/work/topaz/backup/fdt.dts`)
- Active panel (bootargs `msm_drm.dsi_display0=`): `qcom,mdss_dsi_panel_m7_38_0c_0a_fhdp_video`,
  AMOLED (`qcom,mdss-dsi-oled-panel-video-mode`), video mode, burst, 4 lanes, DSC 1.1 (8 bpp,
  slice 540x20), dfps 120/90/60.
- Backlight: `bl_ctrl_dcs`, levels 1..0x7FF (`bl-max-level` = `brightness-max-level` = 0x7FF),
  init level 0x133, `qcom,mdss-dsi-bl-inverted-dbv` (value goes MSB first),
  `qcom,mdss-dsi-bl-dcs-type-ss-ea`, `qcom,bl-update-flag = "delay_until_first_frame"`,
  `qcom,mdss-dsi-dma-trigger = "trigger_sw"`. DT example (doze): `39 01 00 00 00 00 03 51 00 18`
  = DCS long write 0x51 with 2 bytes.
- DSI0 controller `qcom,mdss_dsi_ctrl0@5e94000` (0x400), disp_cc 0x5f08000, mdp intf 0x5e6b800.
- Register offsets: downstream `dsi_ctrl_reg.h` (LineageOS sm6150 kernel, dsi-staging): CTRL 0x004,
  STATUS 0x008, VIDEO_MODE_CTRL 0x010, COMMAND_MODE_DMA_CTRL 0x03C, DMA_CMD_OFFSET 0x048,
  DMA_CMD_LENGTH 0x04C, TRIG_CTRL 0x084, CMD_MODE_DMA_SW_TRIGGER 0x090, INT_CTRL 0x110,
  TEST_PATTERN_GEN_CTRL 0x15C, TEST_PATTERN_GEN_CMD_DMA_INIT_VAL 0x17C, TPG_DMA_FIFO_STATUS 0x1DC,
  TPG_DMA_FIFO_RESET 0x1EC.

## What the hardware looks like under Windows (v0.1 log)
`DSI0: hw 20040001 ctrl 000001f7 status 00002008 video 00009130 dma_ctrl 14000000 trig 00000004
int 00010000 tpg 00000000` — DSI 6G v2.4.1, enabled, video mode, CMD_MODE_EN already set, DMA
trigger = SW, DMA_CTRL has bits 28 and 26 (LOW_POWER).

## Attempts
1. v0.1/v0.2 — **TPG DMA FIFO** path (downstream `dsi_ctrl_hw_cmn_kickoff_fifo_command`: packet
   into TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, TPG_CTRL = BIT1|BIT2|3<<16, FIFO reset, length, SW
   trigger). Packet layout as Linux `dsi_cmd_dma_add` (WC lo, WC hi, DT, flags BIT7 last |
   BIT6 long, payload, 0xff pad). Every send reports CMD_DMA_DONE within 1-8 ms, but **nothing
   reaches the panel**: 0x51 at 20 % and 100 % changed nothing, DCS 0x28 display off for 8 s had
   no visible effect (user checked twice).
2. v0.3 — optional **DMA path** (flag file `C:\topaz\dsi.dma`): packet in a contiguous non-cached
   buffer below 4 GB, DMA_CMD_OFFSET = its PA. Risk: MDSS reaches memory through the apps SMMU
   (0xC600000); a missing mapping = context fault, maybe a frozen picture until reboot. GFSR is
   logged before/after. **Result (2026-10-02 ~21:30): WORKS** — display off/on visibly blanks and
   restores the panel; MDSS reaches the buffer at PA 0xfffff000, apps SMMU GFSR stays 0, DONE in
   1-8 ms. So on this DSI 2.4.1 the TPG FIFO path is a no-op and the memory DMA path is the one.

## Notes
- `C:\topaz\dsicmd` (v0.2+): hex `"<DT> <bytes>"` sent once and deleted; 05 = DCS short write,
  15 = short + 1 param, 39 = long. Display off/on test: `05 28` then `05 29`.
- Windows brightness slider needs the brightness interface of the display adapter
  (DXGK_BRIGHTNESS_INTERFACE) → later in TopazGpuW (step C2); until then the file/tool.

## Built-in Windows slider (2026-10-02 ~21:35)
- Brightness 10 % → 100 % via the DMA path verified by the user. The level now comes from
  `C:\topaz\brightness`; the Windows slider needs the display adapter's DXGK brightness interface.
- The display adapter today is Microsoft Basic Display (no brightness). Our display-only driver
  **TopazDisplay** (KMDOD sample, fixed framebuffer 0x5C000000) is disabled by Windows on every boot
  (System event 4113). DxgKrnl ETW of a restart: StartDevice OK, then
  **`DdiPresentDisplayOnly return 0xC0000001`** → StopDevice; also
  `QueryDeviceDescriptor` for EDID offset 128 returns 0xC00000BB ("invalid NTSTATUS"; must be
  STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA) and "Failed to get preferred mode from monitor EDID".
  Present returns 0xC0000001 either from `IsDriverActive() == FALSE` or from the blt worker path
  (blthw.cxx); BasicDisplay is still enabled and scans out of the same memory.
- Plan: fix TopazDisplay (log the present path, EDID descriptor status, preferred timing), give it
  DXGK_BRIGHTNESS_INTERFACE that sends DCS 0x51 through the DSI DMA path (code from
  TopazBacklight), then disable BasicDisplay → native slider. Risk: a broken TopazDisplay with
  BasicDisplay disabled = black screen until `devcon enable` over SSH or a reboot.

## State when stopped (2026-10-02 ~21:45)
- **Working:** TopazBacklight v0.3 installed on the phone; `C:\topaz\dsi.dma` present (DMA path on);
  `C:\topaz\brightness` = 100. Changing the file (0..100) changes the panel within ~0.3 s.
  `C:\topaz\dsicmd` sends raw packets (05 28 / 05 29 = display off/on, verified).
- **Built, NOT installed:** TopazDisplay v0.3 (CI run 37047724611): DxgkDdiQueryInterface →
  DXGK_BRIGHTNESS_INTERFACE (GetPossible 0..100, Set → DCS 0x51 via the DSI DMA path, Get), target
  reported as D3DKMDT_VOT_INTERNAL, logs for failing PresentDisplayOnly / unknown QueryAdapterInfo.
- **Next:** install TopazDisplay v0.3 (device ROOT\DISPLAY\0000 is not started → update is safe),
  read C:\TopazDisplay.log to see why PresentDisplayOnly fails (0xC0000001), fix, then disable
  BasicDisplay so TopazDisplay owns the panel and the Windows slider appears; afterwards remove
  TopazBacklight (two drivers must not drive DSI0 at the same time) or make TopazBacklight v0.4
  DMA-only for the file control. TODO TopazBacklight v0.4: DMA path by default (flag file not needed).

## TopazDisplay v0.3 on the phone (2026-10-02 ~21:30-21:45, session 2)
- `devcon update TopazDisplay.inf Root\TopazDisplay` (hardware ID, **not** the instance ID
  `ROOT\DISPLAY\0000` — that gives "Unable to find any matching devices"). DxgKrnl keeps the driver
  image loaded after the first start: the new .sys is only picked up after a **reboot**
  (log showed no v0.3 banner until then). Device restarts (`devcon restart Root\TopazDisplay`) re-run
  AddDevice/StartDevice but not DriverEntry — fine for knobs read at start. The log file is held open by
  the driver, so `Remove-Item C:\TopazDisplay.log` fails silently (it keeps growing; read with `-Tail`).
- v0.3 after reboot: Windows disables it again (problem 43). The v0.3 log has no PresentDisplayOnly line
  at all → Present is never reached; the failure is earlier.
- DxgKrnl ETW (`logman create trace dxt -ets -o C:\topaz\dxt.etl -p "{802EC45A-1E99-4B83-9920-87C98277BA9D}"
  0xFFFFFFFFFFFFFFFF 5`, restart the device, `logman stop dxt -ets`, `tracerpt ... -of CSV`; event names via
  `(Get-WinEvent -ListProvider Microsoft-Windows-DxgKrnl).Events`: 118 AddDevice, 138 StartDevice, 148
  QueryChildRelations, 494 text messages). Event 494 "Adapter StartDevice has completed with status
  0xC00000BB" shows up right after QueryChildRelations in the v0.3 trace, **but it also shows up in the
  traces of a healthy start (v0.4, device OK) — it is not the failure cause**; do not chase it.
- v0.3 child descriptor had InterfaceTechnology = D3DKMDT_VOT_INTERNAL (0x80000000) vs v0.2 OTHER.
  v0.4 (this commit) reports the panel as **D3DKMDT_VOT_LVDS** (docs: "LVDS or MIPI DSI");
  `C:\topaz\td.cfg` = "<vot> <hpd>" (decimal, signed; OTHER=-1, LVDS=6, DISPLAYPORT_EMBEDDED=11, hpd 4 =
  Interruptible) overrides at every QueryChildRelations (read at device start, so `devcon restart` is enough).
- v0.4 result: after the reboot the device showed Error (not started, no 4113 event) until a manual
  `devcon restart Root\TopazDisplay`; then **Status OK, problem code 0**, no PresentDisplayOnly failure line in
  the log, no new 4113. Win32_VideoController lists only "Redmi Note 12 Display (topaz)"; monitors:
  "Generic Monitor" (current), "Generic Monitor (Topaz Panel)" (ghost, DISPLAY\TPZ6225). WmiMonitorBrightness
  is still "not supported" (BasicDisplay is still the active adapter for DWM). Open: why it is not started at boot.

## BasicDisplay disable attempt #1 (2026-10-02 ~21:50) — FAILED, reverted
- Method: `ConfigFlags=1` on `HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\BASICDISPLAY\0000` + reboot (a hot
  `devcon disable` of an adapter DWM holds risks a PnP hang). Result: the panel **froze on the boot logo**
  (the bootloader's "scanning disk" screen) — nobody scans out; Windows itself booted (SSH ok).
- At boot TopazDisplay fails to start: Kernel-PnP/Configuration event **411, problem status 0xC00000E5
  (STATUS_INTERNAL_ERROR)** for ROOT\DISPLAY\0000 (seen after every reboot with v0.3 and v0.4); a manual
  `devcon restart Root\TopazDisplay` afterwards starts it (Status OK), but even then the screen stayed on the
  logo — TopazDisplay does not draw anything (DWM is not moved to it / no Present reaches the framebuffer).
- Recovery that worked over SSH: `Set-ItemProperty ...Enum\ROOT\BASICDISPLAY\0000 ConfigFlags 0` and
  `devcon enable "@ROOT\BASICDISPLAY\0000"` (BasicDisplay came back OK without a reboot; running a .ps1 needs
  `-ExecutionPolicy Bypass`).
- Boot-time DxgKrnl ETW autologger installed: `HKLM\...\Control\WMI\Autologger\TopazDxg` → `C:\topaz\boot.etl`
  (provider {802EC45A-1E99-4B83-9920-87C98277BA9D}, level 5), to see why the first start returns 0xC00000E5.
- ScanDisk at boot disabled by the user's request: `chkntfs /x C:` → BootExecute `autocheck autochk /k:C *`
  (C: is flagged dirty from the forced reboots; never ntfsfix it).

## Boot ETW of TopazDisplay v0.4 (autologger, C:\topaz\boot.etl.001, 2026-10-02 ~22:00)
- Our adapter: AddDevice early, StartDevice after BasicDisplay + BasicRender. DDI StartDevice returns OK, then
  DxgKrnl: 148/149 QueryChildRelations (vot=6 hpd=4), 494 "StartDevice completed 0xC00000BB" (**also present in
  the healthy manual-restart trace → benign log text**), QueryChildStatus (event 272, connected), then PnP
  IRPs QUERY_DEVICE_RELATIONS and **IRP_MN_SURPRISE_REMOVAL (0x17)** → DxgKrnl StopDevice. PnP event 411 =
  start failed with 0xC00000E5 (internal error) *after* the DDI start; the same sequence in a manual restart
  does not end in surprise removal.
- "Driver returned an invalid NTSTATUS code 0xC00000BB" right after is **BasicDisplay's own** EDID query
  (event 152/153 on BasicDisplay's adapter, offset 128) — unrelated noise.
- Boot-only difference to find: v0.2 (child tech OTHER, no QueryInterface DDI, no TopazBlInit) got through boot
  start and failed later in Present (0xC0000001/4113); v0.3/v0.4 fail already at start. Suspects: child tech
  INTERNAL/LVDS, DxgkDdiQueryInterface, TopazBlInit. Next experiment: `C:\topaz\td.cfg` = `-1 4` (OTHER, as
  v0.2) and reboot — if the boot start succeeds, the technology value is the cause.

## Present failure found (2026-10-02 ~22:10, TopazDisplay v0.5 log + boot ETW with td.cfg `-1 4`)
- With child technology OTHER (`C:\topaz\td.cfg` = `-1 4`) the boot start succeeds (DxgKrnl asks QueryInterface
  for the brightness GUID {fde5bba4-...}: "provided"), then the first **PresentDisplayOnly returns 0x103
  (STATUS_PENDING)** and DxgKrnl stops the adapter (ETW "DdiPresentDisplayOnly return 0xC0000001", 4113).
  Cause: the KMDOD sample alternates sync/async presents (`BDD_HWBLT::m_SynchExecution`); the async path starts a
  worker thread and reports completion with a fake DXGK_INTERRUPT_DISPLAYONLY_PRESENT_PROGRESS via
  DxgkCbSynchronizeExecution — not usable for a root-enumerated device without an interrupt. → v0.6: always sync.
- Why nothing was in the log before: Present runs at IRQL != PASSIVE and `LogWrite` skipped those. v0.5 added a ring
  buffer (`log.c`, `LogFlush()` on StopDevice).
- With child technology LVDS/INTERNAL (the DSI-correct values) the boot start itself ends in surprise removal
  (event 411 0xC00000E5); not understood yet — retest after the sync fix. Brightness interface is requested by
  DxgkCbs even for technology OTHER, so the slider may not need an INTERNAL type.

## TopazDisplay v0.6 boots and presents (2026-10-02 ~22:20)
- v0.6 (sync Present) + `td.cfg` `-1 4`: after a reboot ROOT\DISPLAY\0000 is **OK**, 20+ Present calls return 0,
  Win32_VideoController "Redmi Note 12 Display (topaz)" 1080x2400, DWM moved to the TPZ6225 monitor ("Generic
  PnP Monitor", EDID size 7x16 cm) while BasicDisplay is still enabled (`Generic Monitor` also listed, OK).
  DxgKrnl asked the adapter for the brightness interface: "QueryInterface brightness v1: provided".
- User-visible: desktop is portrait now (BasicDisplay's desktop was configured differently), picture shows artifacts
  and Windows seems to flip the scale between 100 % and 175 % (two monitor configs / EDID physical size 70x155 mm).
- `WmiMonitorBrightness` still "not supported": monitor.sys only creates it for an internal connector → next:
  retry child technology LVDS (delete `td.cfg`) now that Present works (the earlier LVDS boot failure may have been
  the same async-Present path; unproven).

## BasicDisplay disabled, TopazDisplay v0.6 owns the panel (2026-10-02 ~22:30) — WORKS
- Two displays (BasicDisplay + TopazDisplay) both scanned the same framebuffer → artifacts and scale flipping
  (100 % / 175 %); dragging a window past the edge came back on the same screen.
- `ConfigFlags=1` on `Enum\ROOT\BASICDISPLAY\0000` + reboot: ROOT\DISPLAY\0000 OK at boot, only monitor
  "Generic Monitor (Topaz Panel)" left, 1080x2400. The user set 175 % and rotated the screen — picture is fine.
  Undo (if the screen ever stays on the logo): over SSH `Set-ItemProperty ...Enum\ROOT\BASICDISPLAY\0000
  ConfigFlags 0 -Type DWord` + `devcon enable "@ROOT\BASICDISPLAY\0000"`.
- Still open: slider (WmiMonitorBrightness needs an internal connector type) → retry LVDS (td.cfg removed).

## LVDS retest with the working Present (2026-10-02 ~22:20) — FAILED again
- BasicDisplay disabled, `td.cfg` = `6 4` (LVDS, HPD interruptible) → at boot the adapter is stopped right after
  QueryChildRelations (log: `QueryChildRelations: vot 6 hpd 4` then `StopDevice`), screen frozen on the logo. A
  `devcon restart Root\TopazDisplay` returns the device to OK but DWM does not get the display back; a reboot is
  needed (the first `shutdown /r` after that hung for minutes in "shutdown in progress").
- ETW comparison: with technology OTHER no "StartDevice completed 0xC00000BB" message is produced at all; with
  LVDS/INTERNAL DxgKrnl itself fails StartDevice with **STATUS_NOT_SUPPORTED right after the child relations**
  (so that message *is* the failure for non-OTHER types; my earlier "benign" note applies only to manual
  restarts with BasicDisplay present).
- Ideas to try (each needs a reboot; recovery = `td.cfg` `-1 4` over SSH + reboot): HPD AlwaysConnected (`6 1`),
  AcpiUid != 0 / an ACPI-enumerated adapter (internal connectors may need an ACPI _DOD child — the other session's
  ACPI GPU0 route), D3DKMDT_VOT_DISPLAYPORT_EMBEDDED (11).
- Mac disk was full during this session (ENOSPC); other sessions' scratch files live in /private/tmp/claude-501.

## Research agent report (dxgkrnl/monitor.sys 10.0.22621.1 disassembled with public PDBs, s8build ~/work/dxgre)
- Root cause of the INTERNAL/LVDS failure: `dxgkrnl!DpiFdoEnumChildDevices` rejects INTERNAL, LVDS(6),
  DISPLAYPORT_EMBEDDED(11) and UDI_EMBEDDED(13) children with STATUS_NOT_SUPPORTED (0xC00000BB) unless the adapter
  is the POST device, or BasicDisplay on ROOT\BasicDisplay, or `InitialData.Version < 0x4000` (we build WDDM3_1),
  or the adapter driver key has `SoftGPUAdapter != 0`. HPD awareness and AcpiUid are NOT part of the check, so
  `6 1`, `11 4`, AcpiUid != 0 will not help. POST is detected by `DpiFdoDetectPostDevice` (VMBus device, or
  resources covering the GOP framebuffer, or any ACPI device unless `GraphicsDrivers\DisableAutoAcpiPostDevice`);
  our root adapter is none of these (hence AcquirePostDisplayOwnership returns zeros). An ACPI-enumerated adapter
  would automatically be POST.
- monitor.sys creates WmiMonitorBrightness / `\\.\LCD` if the connector type is INTERNAL/eDP/UDI-embedded **or the
  monitor's driver key has `BrightnessControl` bit0 = 1**
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-brightness-controls-for-external-display-connectors).
  dxgkrnl's brightness IOCTL handler does not check for an internal monitor.
- GUIDs asked by DxgkCbs: 14f9db8b MIPI_DSI, 462bc153 GPU_PARTITION, 197a4a6e/148a3c98/fde5bba4 BRIGHTNESS v3/v2/v1,
  2d09818e DP, 962639f3 DISPLAY_DIAGNOSTICS, 2564aa4f I2C; QAI 20 DISPLAYID_DESCRIPTOR, 29 WDDMDEVICECAPS, 34
  PHYSICAL_MEMORY_CAPS — none decides "internal".
- Plan: (1) `reg add HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-e325-11ce-bfc1-08002be10318}\0001
  /v BrightnessControl /t REG_DWORD /d 1` (0001 = driver key of DISPLAY\TPZ6225\1&28A6823A&0&UID0), then reboot;
  success = WmiMonitorBrightness returns 101 levels, `WmiSetBrightness` logs "brightness N% ... done", slider in
  Quick Settings. Undo: `reg delete ... /v BrightnessControl` + reboot. Durable form: own monitor INF for
  MONITOR\TPZ6225 with `HKR,,BrightnessControl,0x00010001,1`. (2) Fallback: `InitialData.Version =
  DXGKDDI_INTERFACE_VERSION_WIN8 (0x300E)` + `td.cfg` `6 4`. (3) Do not use SoftGPUAdapter. Then remove
  TopazBacklight (two drivers on DSI0).
- Step 1 applied on the phone at ~22:40 (value set, reboot pending user's OK).

## RESULT (2026-10-02 ~22:50): Windows brightness slider WORKS
- `BrightnessControl=1` in the monitor's driver key (`Class\{4d36e96e-...}\0001`, monitor DISPLAY\TPZ6225) + reboot,
  TopazDisplay v0.6 with child technology OTHER (`td.cfg` `-1 4`), BasicDisplay disabled: the slider appeared in
  Windows (user confirmed). `WmiMonitorBrightness`: 101 levels, current 100. Moving the slider logs
  `brightness N% -> level 0x... : done` in C:\TopazDisplay.log (0 % -> level 1, 100 % -> 0x7FF).
- TopazBacklight (Root\TopazBacklight, ROOT\SYSTEM\0005) disabled with `devcon disable` so that only
  TopazDisplay drives DSI0 (re-enable: `devcon enable Root\TopazBacklight`; file control via C:\topaz\brightness
  works again then, but then both write DSI0).
- Not yet durable: `BrightnessControl` and `td.cfg` are manual. TODO: (a) default child technology = OTHER in code
  (LVDS/INTERNAL is rejected by dxgkrnl for non-POST root adapters, see research report) and drop the td.cfg
  need; (b) monitor INF for MONITOR\TPZ6225 with `HKR,,BrightnessControl,0x00010001,1` (or set the value from the
  installer) so a monitor re-enumeration keeps the slider; (c) TopazBacklight v0.4 = DMA path by default, kept as
  a fallback for adapters without TopazDisplay; (d) installer step: disable BasicDisplay (ConfigFlags=1) —
  recovery over SSH documented above.
