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
