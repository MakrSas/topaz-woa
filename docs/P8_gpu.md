# P8: GPU (Adreno 610) for Windows — research and plan (2026-10-01)

## Why
Without a render driver Windows composes on the CPU: `TopazDisplay` is display-only (KMDOD), so DWM
and every D3D app run on WARP. Measured over SSH: `dwm` ~0.7 core at idle UI; a Flutter app with blur
(FlClash) drops the whole screen to ~2 fps while its window is open; Windows animations and
transparency are choppy. The CPU part is done (TopazCpu: gold cluster 300 → 2803 MHz); smooth
animations need the GPU.

## Path 1 (Qualcomm's Windows driver) — rejected
- Package: WOA-Project/Qualcomm-Reference-Drivers `7180_CLS/200.0.32.0/qcdx7180.cab` (32 174 023 B,
  Snapdragon 7c = SC7180, Adreno 618) + `qcpep.wd7180.cab`. Copies on s8build `~/work/gpu/`
  (proprietary — never commit them).
- INF: `ACPI\QCOM083A` (+ SUBSYS CLS07180/IDP07180/CLS08180...). KMD `qcdxkm7180.sys` is shared with
  850/8180/8280 (firmware names `qcdxkmsuc{7180,8180,8280,850}.mbn`), UMD `qcdx11/12arm64xum7180.dll`,
  compiler `qcdxarm64xcompiler7180.dll`.
- The KMD is built around a **GMU** (`GmuPwrStart`, `GmuPwrRequestPowerOn`, `GmuPwrGxBwVote`,
  `GmuPwrQueryGxPowerState`, `DisableGmuACD`...). Every chip it targets has one (A618/630/680/690).
  **A610 has no GMU** (Linux drives it through a "GMU wrapper"). Making it work = reverse engineering
  the KMD or emulating a GMU, plus the SC7180 PEP (clock/rail topology of another SoC). Not worth it.
- The UMD/compiler contain the A610 chip id (shared Qualcomm compiler), but the UMD↔KMD private
  protocol is undocumented, so the UMD cannot be reused with our own KMD either.

## Path 2 (own driver on Mesa freedreno) — chosen
| Stage | What | Cost |
|---|---|---|
| G1 | "GPU alive": KMDF test driver `TopazGpu` — clocks/GDSC on, GPU SMMU identity bank, zap shader via TZ PAS, SQE microcode, ring buffer, CP_ME_INIT, CP_MEM_WRITE test, IRQ | ~1 day |
| G2 | WDDM **render-only** KMD (TopazDisplay stays the display-only adapter; Windows copies frames across adapters): allocations in system RAM, GPU VA = PA (identity SMMU; buffers below 4 GB phys: A610 has `ADRENO_QUIRK_4GB_VA`), DxgkDdiSubmitCommand → ring, IRQ → fences, reset on hang | days |
| G3 | UMD: Mesa `src/gallium/frontends/d3d10umd` (D3D10 DDI, used by VMware's Windows driver with svga) + freedreno gallium (A610 is upstream) + a new freedreno winsys over WDDM callbacks (pfnAllocateCb / pfnRenderCb / escapes) instead of msm DRM ioctls; Mesa built for Windows ARM64 in CI | days–weeks |
| G4 | DWM on the GPU, then tuning (UBWC, GMEM, clocks/CX vote via RPM) | — |

## Hardware facts (stock DT `s8build ~/work/topaz/backup/fdt.dts` + mainline `sm6115.dtsi`)
- GPU `qcom,kgsl-3d0@5900000`: regs 0x5900000 (0x90000) + cx_dbgc 0x5961000 (0x800); IRQ SPI 0xB1
  (177) level high → GSIV 209; chipid **0x06010001** "Adreno610v2"; nvmem speed_bin/gaming_bin;
  initial pwrlevel 6; DDR bus table; `vdd-supply` = gpu_gx_gdsc, `vddcx-supply` = gpu_cx_gdsc.
- GPU clock controller 0x5990000 (gpucc-khaje; Linux `drivers/clk/qcom/gpucc-sm6115.c` is the
  closest): **GPU_CX_GDSC** gdscr 0x106c (abs 0x599106c, votable), **GPU_GX_GDSC** gdscr 0x100c
  (abs 0x599100c, CLAMP_IO | SW_RESET, parent CX); syscons gx_domain_addr 0x5991508, cx_hw_ctrl
  0x5991540, gx_sw_reset 0x5991008. Both GDSCs' parent supply = `pm6125_s3_level` (RPM) — the GPU runs
  on VDD_CX (mainline: `power-domains = <&rpmpd SM6115_VDDCX>`); CX is on already, high GPU clocks
  will need an RPM level vote later.
- GMU wrapper (mainline) 0x596a000, 0x30000.
- GPU SMMU (`kgsl-smmu`, mainline `adreno_smmu`) 0x59a0000, TBU 0x59c5000; mainline
  `iommus = <&adreno_smmu 0 1>`.
- Clock names in the stock DT: core_clk (gpucc gx_gfx3d), rbbmtimer, iface, ahb (gpucc), mem_clk
  (gcc gpu_memnoc_gfx), gmu_clk, smmu_vote, apb_pclk, gpu_cc_ahb, gcc_gpu_memnoc_gfx,
  gpu_cc_hlos1_vote_gpu_smmu, gcc_gpu_snoc_dvm_gfx (map to offsets with gcc-sm6115.c / gpucc-sm6115.c).
- Zap shader memory region: **0x55B15000, 0x2000** (inside Mu's "PIL Reserved" 0x4AB00000+0xB800000,
  so Windows never uses it). GPU PAS id **13** (Linux `GPU_PAS_ID`).
- Firmware: SQE `a630_sqe.fw` (linux-firmware, 34 188 B; s8build `~/work/gpu/a630_sqe.fw`); zap
  `a610_zap.mdt/.bNN` from the vendor partition (`/firmware` or `/firmware-mbn`): ColorOS repack
  `images/super.img`, vendor_a at offset 6343884800, length 691810304 (LP metadata parser
  `~/work/gpu/lp.py`); extracted image `~/work/gpu/vendor_a.img`.

## Linux reference (s8build `~/work/topaz/linux/drivers/gpu/drm/msm/adreno/`)
- `a6xx_catalog.c:683` A610 entry: SQE a630_sqe.fw, zap a610_zap.mdt, gmem 128K+4K, quirk 4GB_VA,
  hwcg `a612_hwcg`, protect `a630_protect`, gbif_cx `a640_gbif`, gmu_cgc_mode 0x00020202,
  prim_fifo_threshold 0x00080000, funcs `a6xx_gmuwrapper_funcs`.
- `a6xx_gpu.c`: `hw_init` 1173 (register setup; a610 specifics: GBIF_QSB_SIDE0..3 = 0x00071620,
  RBBM_GBIF_CLIENT_QOS_CNTL 3, CP_ROQ_THRESHOLDS 0x00800060 / 0x40201b16, CP_MEM_POOL_SIZE 48,
  RBBM_INTERFACE_HANG_INT_CNTL (1<<30)|0x3ffff), `a6xx_ucode_load` 1046, `a6xx_cp_init` 895,
  `a6xx_pm_resume` 2158, gmuwrapper funcs 2824; zap: `adreno_zap_shader_load` in adreno_gpu.c.

## G1 plan in detail
1. GCC GPU clocks on (cfg_ahb, memnoc_gfx, snoc_dvm_gfx), GPU CC: CX GDSC on, AHB/CXO/smmu-vote
   clocks, PLL0 configured + locked, GX_GFX3D RCG at a low frequency, GX GDSC on. Verify GDSC
   PWR_ON status bits before touching 0x5900000 — **reading GPU registers with clocks or GDSC off
   hangs the bus** (same trap as the WLAN CE registers before WLAN_MODE).
2. GPU SMMU 0x59a0000: identity context bank (as TopazModem v0.7 did for the apps SMMU: clone a UEFI
   bank, SCTLR M=0, S2CR type 0 → bank) for the GPU stream ids.
3. Zap: copy `a610_zap.*` to `C:\topaz\fw\image\`, TZ PAS init/mem-setup/auth-and-reset with id 13
   (TopazModem `ModemPas.c` has the SCM calls), region 0x55B15000.
4. `hw_init` register list for A610, CP protect, UBWC config; SQE into a 4 KB-aligned buffer below
   4 GB → CP_SQE_INSTR_BASE; 32 KB ring → CP_RB_BASE / CP_RB_CNTL / CP_RB_RPTR_ADDR; CP_SQE_CNTL = 1;
   CP_ME_INIT (a6xx_cp_init); CP_SET_SECURE_MODE 0 (needs the zap); wait RPTR == WPTR.
5. Test: CP_MEM_WRITE magic → read back from RAM; CP_EVENT_WRITE + RBBM IRQ mask → see SPI 177.
   Log `C:\TopazGpu.log`; deploy over SSH like TopazCpu.

## Risks
- GPU CC PLL programming (alpha PLL config from gpucc-sm6115.c) and the CX level for the chosen clock.
- The GPU SMMU might have secure banks owned by TZ/hyp; the identity trick worked on the apps SMMU.
- WDDM render-only + display-only cross-adapter path on ARM64 with our KMDOD display driver.
- Mesa on Windows ARM64: d3d10umd is maintained for VMware; freedreno has never run on Windows.
